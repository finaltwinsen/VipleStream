/**
 * @file src/self_update.cpp
 * @brief VipleStream §SELF-UPDATE — host 端自我更新實作（設計說明見 self_update.h）。
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <charconv>
#include <deque>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

// lib includes
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

// local includes
#include "self_update.h"
#include "config.h"
#include "entry_handler.h"
#include "logging.h"
#include "platform/common.h"
#include "rtsp.h"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <unistd.h>

  // 由 cmake/compile_definitions/common.cmake 從 src_assets/linux/misc/self-update-helper.sh 產生（kLinuxScript 的來源）
  #include "self_update_helper_script.h"
#endif

using namespace std::literals;

namespace self_update {

  namespace {
    constexpr auto kRepo = "finaltwinsen/VipleStream"sv;
    constexpr auto kRepoTagPrefix = "https://github.com/finaltwinsen/VipleStream/releases/tag/"sv;
    constexpr auto kLatestPage = "https://github.com/finaltwinsen/VipleStream/releases/latest"sv;
    constexpr auto kApiBase = "https://api.github.com/repos/finaltwinsen/VipleStream/releases"sv;
    constexpr auto kUserAgent = "VipleStream-Server-SelfUpdate/2.0 (" PROJECT_VERSION ")";
    constexpr auto kResultFile = "self_update.result"sv;

    std::atomic<state_e> g_state {state_e::idle};
    std::atomic<bool> g_busy {false};
    std::atomic<bool> g_cancel {false};
    // §REVIEW-1111：下載期間有人開始串流而放棄的那一次安裝；自動安裝看到就把「這一版試過了」清掉
    std::atomic<bool> g_deferred_by_stream {false};
    std::mutex g_pending_mtx;
    std::optional<release_info> g_pending;
    std::mutex g_worker_mtx;
    std::thread g_worker;

    std::string current_version() {
      return std::string(PROJECT_VERSION);
    }

    std::string asset_name_for(const std::string &version) {
#ifdef _WIN32
      return "VipleStream-Server-" + version + ".zip";
#else
      return "VipleStream-Server-" + version + "-linux-x64.deb";
#endif
    }

    std::string label_for(state_e st, const std::optional<release_info> &pend) {
      switch (st) {
        case state_e::checking:
          return "Checking for updates...";
        case state_e::update_available:
          return pend ? ("Install update v" + pend->version) : "Check for updates...";
        case state_e::downloading:
        case state_e::installing:
          return "Updating...";
        default:
          return "Check for updates...";
      }
    }

    void set_state(state_e st, const state_fn &on_state) {
      g_state.store(st);
      if (on_state) {
        std::optional<release_info> pend;
        {
          std::lock_guard<std::mutex> lg(g_pending_mtx);
          pend = g_pending;
        }
        const bool busy = (st == state_e::checking || st == state_e::downloading || st == state_e::installing);
        on_state(busy, label_for(st, pend));
      }
    }

    std::filesystem::path result_path() {
      return platf::appdata() / std::string(kResultFile);
    }

    std::string log_tail(const std::filesystem::path &file, int lines) {
      std::ifstream in(file);
      if (!in) return {};
      if (lines <= 0) return {};
      std::deque<std::string> ring;
      std::string line;
      while (std::getline(in, line)) {
        if ((int) ring.size() >= lines) ring.pop_front();
        ring.push_back(std::move(line));
      }
      std::string out;
      for (const auto &l : ring) {
        out += l;
        out += '\n';
      }
      return out;
    }

#ifdef _WIN32
    std::filesystem::path install_dir() {
      wchar_t buf[MAX_PATH];
      if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) {
        return std::filesystem::current_path();
      }
      return std::filesystem::path(buf).parent_path();
    }

    bool dir_writable(const std::filesystem::path &dir) {
      std::error_code ec;
      std::filesystem::create_directories(dir, ec);
      if (ec) return false;
      const auto probe = dir / (".probe-" + std::to_string(GetCurrentProcessId()));
      FILE *fp = _wfopen(probe.c_str(), L"wb");
      if (!fp) return false;
      fclose(fp);
      std::filesystem::remove(probe, ec);
      return true;
    }

    // 更新工作目錄：優先 <install>\config\update（Program Files：只有 Admin/SYSTEM 可寫，
    // SYSTEM service 用這裡不會被一般使用者預佈檔案劫持）；standalone 且不可寫時退到
    // %LOCALAPPDATA%\VipleStream\update（使用者私有）。絕不用 C:\Windows\Temp。
    std::filesystem::path update_dir() {
      auto dir = install_dir() / "config" / "update";
      if (dir_writable(dir)) return dir;
      if (const char *la = std::getenv("LOCALAPPDATA"); la && *la) {
        dir = std::filesystem::path(la) / "VipleStream" / "update";
        if (dir_writable(dir)) return dir;
      }
      return {};
    }
#else
    // Linux：<config>/update，0700，驗 owner 與非 symlink（root 會執行裡面的腳本）
    std::filesystem::path update_dir() {
      std::error_code ec;
      auto dir = platf::appdata() / "update";
      std::filesystem::create_directories(dir, ec);
      std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
      struct stat st {};
      if (lstat(dir.c_str(), &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode) ||
          st.st_uid != geteuid() || (st.st_mode & 077) != 0) {
        BOOST_LOG(warning) << "[VIPLE-UPDATE] update dir not private/owned: " << dir.string();
        return {};
      }
      return dir;
    }
#endif

    size_t curl_write_string(char *ptr, size_t size, size_t nmemb, void *userdata) {
      auto *out = static_cast<std::string *>(userdata);
      out->append(ptr, size * nmemb);
      return size * nmemb;
    }

    int curl_xferinfo(void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
      return g_cancel.load() ? 1 : 0;  // 非 0 → CURLE_ABORTED_BY_CALLBACK
    }

    void curl_common(CURL *curl) {
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);  // NOSONAR
      curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_xferinfo);
#ifdef _WIN32
      // MSYS2 的 curl 是 OpenSSL 後端，預設 CA bundle 路徑只存在於 msys 環境，部署後
      // 一律 curl 77「Problem with the SSL CA cert」。改用 Windows 憑證存放區驗證。
      curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long) CURLSSLOPT_NATIVE_CA);
#endif
      static std::once_flag logged;
      std::call_once(logged, [] {
        BOOST_LOG(info) << "[VIPLE-UPDATE] " << curl_version();
      });
    }

    std::string curl_error_text(CURLcode rc, long code) {
      std::ostringstream ss;
      if (rc != CURLE_OK) {
        ss << "curl " << rc << " (" << curl_easy_strerror(rc) << ")";
        if (code > 0) ss << " HTTP " << code;
      }
      else {
        ss << "HTTP " << code;
      }
      return ss.str();
    }

    bool parse_version_parts(const std::string &s, std::vector<int> &out) {
      out.clear();
      std::stringstream ss(s);
      std::string part;
      while (std::getline(ss, part, '.')) {
        if (part.empty() || part.size() > 9 || part.find_first_not_of("0123456789") != std::string::npos) {
          return false;
        }
        int v = 0;
        auto res = std::from_chars(part.data(), part.data() + part.size(), v);
        if (res.ec != std::errc() || res.ptr != part.data() + part.size()) return false;
        out.push_back(v);
      }
      return out.size() >= 2;
    }

    std::optional<std::string> parse_tag_from_location(const std::string &location) {
      // 只接受本 repo 的 tag 頁；302 帶去別處一律拒絕
      if (location.rfind(std::string(kRepoTagPrefix), 0) != 0) return std::nullopt;
      std::string tag = location.substr(kRepoTagPrefix.size());
      const auto cut = tag.find_first_of("/?#");
      if (cut != std::string::npos) tag = tag.substr(0, cut);
      if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
      std::vector<int> parts;
      if (!parse_version_parts(tag, parts)) return std::nullopt;
      return tag;
    }

    release_info make_release(const std::string &version) {
      release_info rel;
      rel.version = version;
      rel.page_url = std::string(kRepoTagPrefix) + "v" + version;
      rel.asset_name = asset_name_for(version);
      rel.asset_url = "https://github.com/" + std::string(kRepo) + "/releases/download/v" + version + "/" + rel.asset_name;
      return rel;
    }

    // 主通道：HEAD releases/latest → 302 Location 取 tag（免 API 配額）
    std::optional<release_info> query_via_head(std::string *error) {
      CURL *curl = curl_easy_init();  // NOSONAR
      if (!curl) {
        if (error) *error = "curl init failed";
        return std::nullopt;
      }
      curl_common(curl);
      curl_easy_setopt(curl, CURLOPT_URL, std::string(kLatestPage).c_str());
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
      const CURLcode rc = curl_easy_perform(curl);
      long code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
      char *redirect = nullptr;
      curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirect);
      const std::string location = redirect ? redirect : "";
      const std::string err = curl_error_text(rc, code);
      curl_easy_cleanup(curl);

      if (rc != CURLE_OK || code < 300 || code >= 400 || location.empty()) {
        if (error) *error = "HEAD " + std::string(kLatestPage) + ": " + err;
        return std::nullopt;
      }
      auto tag = parse_tag_from_location(location);
      if (!tag) {
        if (error) *error = "HEAD redirect is not a release tag of this repo: " + location;
        return std::nullopt;
      }
      return make_release(*tag);
    }

    std::optional<nlohmann::json> api_get_json(const std::string &url, std::string *error, long *http_code, long timeout_s = 30L) {
      CURL *curl = curl_easy_init();  // NOSONAR
      if (!curl) {
        if (error) *error = "curl init failed";
        return std::nullopt;
      }
      std::string body;
      struct curl_slist *headers = nullptr;
      headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
      headers = curl_slist_append(headers, "X-GitHub-Api-Version: 2022-11-28");
      curl_common(curl);
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_string);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s);
      const CURLcode rc = curl_easy_perform(curl);
      long code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
      const std::string err = curl_error_text(rc, code);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      if (http_code) *http_code = code;
      if (rc != CURLE_OK || code != 200) {
        if (error) *error = "API " + url + ": " + err + (code == 403 || code == 429 ? " (rate limited?)" : "");
        return std::nullopt;
      }
      auto j = nlohmann::json::parse(body, nullptr, false);
      if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "API JSON malformed";
        return std::nullopt;
      }
      return j;
    }

    std::string json_str(const nlohmann::json &j, const char *key) {
      auto it = j.find(key);
      return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
    }

    bool json_bool(const nlohmann::json &j, const char *key) {
      auto it = j.find(key);
      return it != j.end() && it->is_boolean() && it->get<bool>();
    }

    // ── §UPDATE-NOTES-TRAY（2026-11-20）：「有新版」通知後面接一行更新摘要 ──
    // 摘要＝release 內文裡第一個「## 」標題行（「vX.Y.Z — 一句話主軸」）。內文是雙語：英文段在前，單獨一行
    // 「# 繁體中文」之後是中文段（規則同 client 的 moonlight-qt/app/backend/updatenotes.h）；介面語系 zh 開頭取中文段。
    // 內文要打 GitHub API（匿名每小時 60 次）才拿得到，所以一個版號在這個行程裡只查一次（失敗也算），結果記在這裡；
    // 備援查版（/latest）與安裝前取 digest（/tags/vX）拿到的 JSON 也順手存進來，之後同一版就不用為了摘要再打一次。
    std::mutex g_notes_mtx;
    std::string g_notes_version;  ///< 已經查過內文的版號（查失敗也記，body 留空）
    std::string g_notes_body;

    void remember_notes(const std::string &version, const nlohmann::json &j) {
      std::string body = json_str(j, "body");
      if (body.size() > 256 * 1024) body.resize(256 * 1024);  // 正規的 release notes 遠小於這個上限
      std::lock_guard<std::mutex> lg(g_notes_mtx);
      g_notes_version = version;
      g_notes_body = std::move(body);
    }

    std::string trim_ws(const std::string &s) {
      const auto b = s.find_first_not_of(" \t\r\n");
      if (b == std::string::npos) return {};
      const auto e = s.find_last_not_of(" \t\r\n");
      return s.substr(b, e - b + 1);
    }

    /// 從 release 內文取出這個語系那一段的標題；找不到就回空字串
    std::string notes_headline(const std::string &body, bool want_zh) {
      std::string en, zh;
      bool in_zh = false;
      std::istringstream in(body);
      std::string raw;
      while (std::getline(in, raw)) {
        const std::string line = trim_ws(raw);
        if (!in_zh && !raw.empty() && raw[0] == '#' && raw.compare(0, 2, "##") != 0 && trim_ws(raw.substr(1)) == "繁體中文") {
          in_zh = true;  // 分隔行：單獨一行的一級標題「# 繁體中文」
          continue;
        }
        if (line.compare(0, 3, "## ") != 0) continue;
        std::string &dst = in_zh ? zh : en;
        if (dst.empty()) dst = trim_ws(line.substr(3));
        if (in_zh && !zh.empty()) break;  // 中文段的標題拿到了，後面不用看
      }
      // 沒有分隔行的舊 release 只有一段（落在 en），任何語系都顯示它；那個語系的段落沒有標題就退回另一段
      if (want_zh) return zh.empty() ? en : zh;
      return en.empty() ? zh : en;
    }

    /// UTF-8 字串佔幾個 UTF-16 單位（Windows 的通知文字是 256 個 wchar_t 的固定緩衝，放不下時整段轉換會失敗）
    size_t utf16_units(const std::string &s) {
      size_t n = 0;
      for (unsigned char c : s) {
        if ((c & 0xC0) == 0x80) continue;  // 接續位元組
        n += (c >= 0xF0) ? 2 : 1;
      }
      return n;
    }

    /// 截到最多 max_units 個 UTF-16 單位（不切在一個字的中間）；截過就在尾端加「…」
    std::string clip_utf16(const std::string &s, size_t max_units) {
      if (utf16_units(s) <= max_units) return s;
      if (max_units == 0) return {};
      size_t units = 0;
      size_t i = 0;
      while (i < s.size()) {
        const unsigned char c = (unsigned char) s[i];
        const size_t len = c < 0x80 ? 1 : (c >= 0xF0 ? 4 : (c >= 0xE0 ? 3 : (c >= 0xC0 ? 2 : 1)));
        const size_t w = c >= 0xF0 ? 2 : 1;
        if (i + len > s.size() || units + w > max_units - 1) break;  // 留一格給「…」
        units += w;
        i += len;
      }
      return trim_ws(s.substr(0, i)) + "…";
    }

    /// 這個版號的更新摘要。沒查過就打一次 API（在背景工作執行緒上，最多等 10 秒）；拿不到就回空字串，不影響查版與安裝
    std::string notes_summary_for(const std::string &version) {
      bool known = false;
      std::string body;
      {
        std::lock_guard<std::mutex> lg(g_notes_mtx);
        if (g_notes_version == version) {
          known = true;
          body = g_notes_body;
        }
        else {
          // 先記下「這一版查過了」：下面失敗（沒網路、限流）也不再重查，免得把匿名配額用掉
          g_notes_version = version;
          g_notes_body.clear();
        }
      }
      if (!known) {
        std::string err;
        long code = 0;
        auto j = api_get_json(std::string(kApiBase) + "/tags/v" + version, &err, &code, 10L);
        if (!j) {
          BOOST_LOG(info) << "[VIPLE-UPDATE] release notes for v" << version << " not available (" << err
                          << "); the notification goes without a summary";
          return {};
        }
        remember_notes(version, *j);
        body = json_str(*j, "body");
        if (body.size() > 256 * 1024) body.resize(256 * 1024);
      }
      const bool want_zh = config::sunshine.locale.compare(0, 2, "zh") == 0;
      std::string head = notes_headline(body, want_zh);
      for (char &c : head) {
        const unsigned char u = (unsigned char) c;
        if (u < 0x20 || u == 0x7F) {
          c = ' ';
        }
        else if (c == '<') {
          c = '(';  // Linux 的通知內文可能被通知服務當成標記語言解析
        }
        else if (c == '>') {
          c = ')';
        }
#ifndef _WIN32
        else if (c == '&') {
          c = '+';
        }
#endif
      }
      head = clip_utf16(trim_ws(head), 120);
      BOOST_LOG(info) << "[VIPLE-UPDATE] release notes summary for v" << version << " (" << (want_zh ? "zh" : "en")
                      << "): " << (head.empty() ? "<none>" : head);
      return head;
    }

    /// 把摘要接在通知文字後面（另起一行）；整段不超過 250 個 UTF-16 單位，放不下就不接
    std::string with_summary(const std::string &text, const std::string &summary) {
      if (summary.empty()) return text;
      const size_t used = utf16_units(text) + 1;
      if (used + 16 > 250) return text;
      return text + "\n" + clip_utf16(summary, std::min<size_t>(120, 250 - used));
    }

    // 備援：REST API JSON（匿名 60 次/小時/IP）
    std::optional<release_info> query_via_api(std::string *error) {
      long code = 0;
      auto j = api_get_json(std::string(kApiBase) + "/latest", error, &code);
      if (!j) return std::nullopt;
      if (json_bool(*j, "draft") || json_bool(*j, "prerelease")) {
        if (error) *error = "latest release is draft/prerelease";
        return std::nullopt;
      }
      std::string tag = json_str(*j, "tag_name");
      if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
      std::vector<int> parts;
      if (!parse_version_parts(tag, parts)) {
        if (error) *error = "API tag not version-like: " + tag;
        return std::nullopt;
      }
      remember_notes(tag, *j);  // §UPDATE-NOTES-TRAY：內文已經在這份 JSON 裡，不用再查一次
      return make_release(tag);
    }

    bool looks_like_package(const std::filesystem::path &file) {
      std::error_code ec;
      const auto size = std::filesystem::file_size(file, ec);
      if (ec || size < 1024 * 1024) return false;
      std::ifstream in(file, std::ios::binary);
      char magic[8] = {};
      in.read(magic, sizeof(magic));
#ifdef _WIN32
      return magic[0] == 'P' && magic[1] == 'K' && magic[2] == 3 && magic[3] == 4;
#else
      return std::string_view(magic, 7) == "!<arch>"sv;
#endif
    }

    void write_result(const std::string &text) {
      std::error_code ec;
      std::filesystem::create_directories(platf::appdata(), ec);
      std::ofstream out(result_path(), std::ios::trunc);
      out << text << "\n";
    }

#ifdef _WIN32
    // 內嵌的 Windows 更新腳本（PowerShell 5.1）。
    //   -Mode 由 C++ 提示（service / standalone），腳本再自行核對。
    //   service：確認 Stopped 才動檔；兩段式 rename .old → 覆蓋，失敗回滾；Start-Service 驗 Running。
    //   standalone：解壓成功後寫 ProceedMarker（server 看到才退出）→ 等 ParentPid 結束 → 覆蓋 → 重啟。
    //   非管理員且需要提權：Start-Process -Verb RunAs -Wait，回傳子行程 exit code；取消 → exit 3。
    //   結果寫 config\self_update.result（版號或 "FAILED …"），tray 下次啟動讀取。
    constexpr const char *kWindowsScript = R"PS1(
param(
    [string]$ZipPath,
    [string]$InstallPath,
    [string]$WorkDir,
    [string]$ServiceName = 'VipleStreamServer',
    [int]$ParentPid = 0,
    [string]$Version = '',
    [string]$Sha256 = '',
    [string]$ProceedMarker = '',
    [int]$CallerPid = 0
)
# 不依賴 script module（Get-FileHash / Expand-Archive 在某些環境的 PSModulePath 下載不到）
Add-Type -AssemblyName System.IO.Compression.FileSystem -ErrorAction SilentlyContinue
function Get-Sha256Hex($path) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $fs = [IO.File]::OpenRead($path)
    try { $bytes = $sha.ComputeHash($fs) } finally { $fs.Dispose(); $sha.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}
$ErrorActionPreference = 'Stop'
try { New-Item -ItemType Directory -Force -Path (Join-Path $InstallPath 'config') | Out-Null } catch {}
$logPath = Join-Path $InstallPath 'config\self_update.log'
function Log($m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $m
    try { Add-Content -Path $logPath -Value $line -Encoding UTF8 } catch {}
}
function Write-Result($text) {
    try { Set-Content -Path (Join-Path $InstallPath 'config\self_update.result') -Value $text -Encoding ASCII } catch {}
}
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$serviceMode = $false
try {
    $imagePath = (Get-ItemProperty -Path ("HKLM:\SYSTEM\CurrentControlSet\Services\" + $ServiceName) -ErrorAction SilentlyContinue).ImagePath
    if (-not $imagePath) {
        $svcObj = Get-CimInstance Win32_Service -Filter "Name='$ServiceName'" -ErrorAction SilentlyContinue
        if ($svcObj) { $imagePath = $svcObj.PathName }
    }
    if ($imagePath) {
        $svcExe = ($imagePath -replace '^"([^"]+)".*$', '$1')
        $serviceMode = $svcExe.StartsWith($InstallPath.TrimEnd([char]92) + [string][char]92, [StringComparison]::OrdinalIgnoreCase)
    }
} catch {}
$needAdmin = $serviceMode
if (-not $needAdmin) {
    try { $probe = Join-Path $InstallPath ('.self_update_probe_' + [guid]::NewGuid().ToString('N')); [IO.File]::WriteAllText($probe, 'x'); Remove-Item $probe -Force } catch { $needAdmin = $true }
}
Log "self-update invoked: version=$Version admin=$isAdmin serviceMode=$serviceMode needAdmin=$needAdmin install=$InstallPath work=$WorkDir"
if ($needAdmin -and -not $isAdmin) {
    Log 'elevating via UAC'
    try {
        $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $PSCommandPath + '"'),
                     '-ZipPath',('"' + $ZipPath + '"'),'-InstallPath',('"' + $InstallPath + '"'),'-WorkDir',('"' + $WorkDir + '"'),
                     '-ServiceName',$ServiceName,'-ParentPid',$ParentPid,'-Version',$Version,'-Sha256',$Sha256,
                     '-ProceedMarker',('"' + $ProceedMarker + '"'),'-CallerPid',$CallerPid)
        $p = Start-Process -FilePath 'powershell.exe' -ArgumentList $argList -Verb RunAs -WindowStyle Hidden -Wait -PassThru
        Log ("elevated child exit=" + $p.ExitCode)
        exit $p.ExitCode
    }
    catch {
        Log ('ERROR: elevation cancelled/failed: ' + $_.Exception.Message)
        Write-Result ('FAILED elevation cancelled')
        exit 3
    }
}
$renamed = @()
$stage = ''
try {
    if ($Sha256 -and $Sha256 -ne 'none' -and $Sha256 -ne '-') {
        $h = Get-Sha256Hex $ZipPath
        if ($h -ne $Sha256.ToLowerInvariant()) { Log "ERROR: sha256 mismatch ($h)"; Write-Result 'FAILED sha256 mismatch'; exit 4 }
        Log 'sha256 verified'
    }
    $stage = Join-Path $WorkDir ('stage-' + [guid]::NewGuid().ToString('N'))
    # PowerShell 5.1 在編譯整支腳本時就解析型別字面值，Add-Type 之後的 [ZipFile] 仍會
    # 「找不到類型」；改用 [Type]::GetType 在執行期取型別再呼叫靜態方法。
    $zipType = [Type]::GetType('System.IO.Compression.ZipFile, System.IO.Compression.FileSystem, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089')
    if ($zipType) { $zipType::ExtractToDirectory($ZipPath, $stage) }
    else { Expand-Archive -Path $ZipPath -DestinationPath $stage -Force }
    $exe = Get-ChildItem -Path $stage -Recurse -Filter 'viplestream-server.exe' | Select-Object -First 1
    if (-not $exe) { Log 'ERROR: viplestream-server.exe not found in package'; Write-Result 'FAILED package has no server exe'; exit 2 }
    $root = $exe.DirectoryName
    if (-not $serviceMode -and $ProceedMarker) {
        Set-Content -Path $ProceedMarker -Value 'go' -Encoding ASCII
        Log 'proceed marker written'
    }
    if ($serviceMode) {
        Log 'stopping service'
        Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
        $tries = 0
        while ((Get-Service -Name $ServiceName).Status -ne 'Stopped' -and $tries -lt 30) { Start-Sleep -Seconds 1; $tries++ }
        if ((Get-Service -Name $ServiceName).Status -ne 'Stopped') {
            Log 'service did not stop in 30 s; force killing'
            Get-Process -Name 'viplestream-svc','viplestream-server' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like ($InstallPath + '*') } | Stop-Process -Force -ErrorAction SilentlyContinue
            $tries = 0
            while ((Get-Service -Name $ServiceName).Status -ne 'Stopped' -and $tries -lt 10) { Start-Sleep -Seconds 1; $tries++ }
        }
        if ((Get-Service -Name $ServiceName).Status -ne 'Stopped') { Log 'ERROR: service still not stopped; aborting without touching files'; Write-Result 'FAILED service did not stop'; exit 5 }
    }
    elseif ($ParentPid -gt 0) {
        Log "waiting for pid $ParentPid to exit"
        $tries = 0
        while ((Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) -and $tries -lt 60) { Start-Sleep -Seconds 1; $tries++ }
        if (Get-Process -Id $ParentPid -ErrorAction SilentlyContinue) { Log 'parent still alive; killing'; Stop-Process -Id $ParentPid -Force -ErrorAction SilentlyContinue }
    }
    # 收掉 InstallPath 下其他 server 行程；發起更新的 CLI 行程（CallerPid）除外——它在等我們回報
    Get-Process -Name 'viplestream-server' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like ($InstallPath + '*') -and $_.Id -ne $CallerPid } | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
    Get-ChildItem -Path $InstallPath -Force -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*.old' } | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
    $copied = 0
    Get-ChildItem -Path $root -Force | ForEach-Object {
        if ($_.Name -in @('config','credentials')) { return }
        $dst = Join-Path $InstallPath $_.Name
        $old = $dst + '.old'
        if (Test-Path -LiteralPath $dst) {
            Move-Item -LiteralPath $dst -Destination $old -Force
            $script:renamed += ,@($dst, $old)
        }
        Copy-Item -LiteralPath $_.FullName -Destination $dst -Recurse -Force
        $copied++
    }
    Log "copied $copied top-level entries"
    Write-Result $Version
    if ($serviceMode) {
        Log 'starting service'
        Start-Service -Name $ServiceName
        $tries = 0
        while ((Get-Service -Name $ServiceName).Status -ne 'Running' -and $tries -lt 30) { Start-Sleep -Seconds 1; $tries++ }
        if ((Get-Service -Name $ServiceName).Status -ne 'Running') { throw 'service did not reach Running after update' }
    }
    else {
        Log 'relaunching standalone server'
        Start-Process -FilePath (Join-Path $InstallPath 'viplestream-server.exe') -WorkingDirectory $InstallPath
    }
    foreach ($pair in $renamed) { Remove-Item -LiteralPath $pair[1] -Recurse -Force -ErrorAction SilentlyContinue }
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    # 只刪自己下載到工作目錄的套件；使用者以 CLI 指定的路徑（例如 release 下的 zip）不能動
    if ($ZipPath.StartsWith($WorkDir.TrimEnd([char]92) + [string][char]92, [StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
    }
    Log 'self-update done'
    exit 0
}
catch {
    Log ('ERROR: ' + $_.Exception.Message)
    Log ('rolling back ' + $renamed.Count + ' entries')
    for ($i = $renamed.Count - 1; $i -ge 0; $i--) {
        $dst = $renamed[$i][0]; $old = $renamed[$i][1]
        try {
            if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Recurse -Force -ErrorAction SilentlyContinue }
            Move-Item -LiteralPath $old -Destination $dst -Force
        } catch { Log ('rollback failed for ' + $dst + ': ' + $_.Exception.Message) }
    }
    Write-Result ('FAILED ' + $_.Exception.Message)
    try {
        if ($serviceMode) { Start-Service -Name $ServiceName -ErrorAction SilentlyContinue }
        elseif (-not (Get-Process -Name 'viplestream-server' -ErrorAction SilentlyContinue | Where-Object { $_.Path -like ($InstallPath + '*') })) {
            Start-Process -FilePath (Join-Path $InstallPath 'viplestream-server.exe') -WorkingDirectory $InstallPath
        }
    } catch {}
    if ($stage) { Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue }
    exit 1
}
)PS1";

    bool running_under_service() {
      return GetConsoleWindow() == nullptr;
    }

    struct launched_t {
      HANDLE process = nullptr;
      DWORD pid = 0;
    };

    bool launch_updater(const std::filesystem::path &script, const std::filesystem::path &package, const release_info &rel,
                        const std::filesystem::path &work, const std::filesystem::path &marker, DWORD parent_pid,
                        DWORD caller_pid, launched_t &out, std::string *error) {
      wchar_t sysdir[MAX_PATH];
      std::wstring ps = L"powershell.exe";
      if (GetSystemDirectoryW(sysdir, MAX_PATH) > 0) {
        ps = std::wstring(sysdir) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
      }
      const auto install = install_dir();
      // 空雜湊用 "none"：單獨一個 "-" 會被 PowerShell 當成空名稱的參數（PSArgumentException "name"）
      const std::wstring sha = rel.asset_sha256.empty() ? L"none" : std::filesystem::path(rel.asset_sha256).wstring();
      std::wstring cmd = L"\"" + ps + L"\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + script.wstring() +
                         L"\" -ZipPath \"" + package.wstring() + L"\" -InstallPath \"" + install.wstring() +
                         L"\" -WorkDir \"" + work.wstring() + L"\" -ServiceName VipleStreamServer -ParentPid " +
                         std::to_wstring(parent_pid) + L" -Version " + std::filesystem::path(rel.version).wstring() +
                         L" -Sha256 " + sha + L" -ProceedMarker \"" + marker.wstring() + L"\" -CallerPid " + std::to_wstring(caller_pid);
      STARTUPINFOW si {};
      si.cb = sizeof(si);
      PROCESS_INFORMATION pi {};
      DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT | CREATE_BREAKAWAY_FROM_JOB;
      std::wstring mutable_cmd = cmd;
      BOOL ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, install.c_str(), &si, &pi);
      if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;
        mutable_cmd = cmd;
        ok = CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, flags, nullptr, install.c_str(), &si, &pi);
      }
      if (!ok) {
        if (error) *error = "CreateProcess(powershell) failed: " + std::to_string(GetLastError());
        return false;
      }
      CloseHandle(pi.hThread);
      out.process = pi.hProcess;
      out.pid = pi.dwProcessId;
      return true;
    }

    bool apply_windows(const std::filesystem::path &package, const release_info &rel, bool tray_mode, std::string *error) {
      const auto work = update_dir();
      if (work.empty()) {
        if (error) *error = "no writable update directory";
        return false;
      }
      const auto script = work / "self_update.ps1";
      {
        std::ofstream out(script, std::ios::binary | std::ios::trunc);
        if (!out) {
          if (error) *error = "cannot write " + script.string();
          return false;
        }
        out << kWindowsScript;
      }
      const auto marker = work / ("proceed-" + std::to_string(GetCurrentProcessId()));
      std::error_code ec;
      std::filesystem::remove(marker, ec);
      const bool service = running_under_service();

      // ParentPid 只在「tray 觸發的 standalone server」才給：腳本會等它退出。CLI 行程
      // 本身不是 server，給 0（腳本改用路徑找出正在跑的 server 收掉），否則 CLI 等腳本、
      // 腳本等 CLI 會互等 60 秒。
      const DWORD parent_pid = (tray_mode && !service) ? GetCurrentProcessId() : 0;
      // CLI 行程自己也是 InstallPath 下的 viplestream-server.exe，腳本收行程時要跳過它
      const DWORD caller_pid = tray_mode ? 0 : GetCurrentProcessId();
      launched_t child;
      if (!launch_updater(script, package, rel, work, marker, parent_pid, caller_pid, child, error)) {
        return false;
      }
      BOOST_LOG(info) << "[VIPLE-UPDATE] updater launched (pid " << child.pid << ") script=" << script.string()
                      << " package=" << package.string() << " service=" << service << " tray=" << tray_mode;
      const auto log_file = install_dir() / "config" / "self_update.log";

      if (!tray_mode) {
        // CLI：等腳本結束（含 UAC 等待），回報 exit code 與 log 尾
        const DWORD w = WaitForSingleObject(child.process, 15 * 60 * 1000);
        DWORD code = 0;
        GetExitCodeProcess(child.process, &code);
        CloseHandle(child.process);
        if (w != WAIT_OBJECT_0) {
          if (error) *error = "updater still running after 15 minutes; see " + log_file.string();
          return false;
        }
        if (code != 0) {
          if (error) *error = "updater exited with code " + std::to_string(code) + "\n" + log_tail(log_file, 6);
          return false;
        }
        return true;
      }

      if (service) {
        // service 模式：腳本自己 Stop-Service（會結束本行程與 tray）。
        // §REVIEW-1111：原本一啟動腳本就回成功。腳本在停服務之前失敗（checksum、解壓、找不到 exe）時服務還活著，
        // 狀態卻停在 installing、自動檢查從此不再跑。「我們還活著而腳本已經結束」就是它沒走到停服務：照失敗處理。
        const auto svc_deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
        while (std::chrono::steady_clock::now() < svc_deadline && !g_cancel.load()) {
          if (WaitForSingleObject(child.process, 250) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(child.process, &code);
            CloseHandle(child.process);
            if (error) *error = "updater exited with code " + std::to_string(code) + " before restarting the service\n" + log_tail(log_file, 4);
            return false;
          }
        }
        CloseHandle(child.process);
        if (g_cancel.load()) {
          return true;  // 服務正在被停下來（腳本做的）
        }
        if (error) *error = "updater still running after 10 minutes; see " + log_file.string();
        return false;
      }

      // standalone：等腳本取得權限並解壓成功（proceed 標記）才退出；腳本失敗就不退出
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(240);
      while (std::chrono::steady_clock::now() < deadline && !g_cancel.load()) {
        if (std::filesystem::exists(marker, ec)) {
          std::filesystem::remove(marker, ec);
          CloseHandle(child.process);
          BOOST_LOG(info) << "[VIPLE-UPDATE] updater ready; exiting so it can replace files";
          lifetime::exit_sunshine(0, true);
          return true;
        }
        if (WaitForSingleObject(child.process, 250) == WAIT_OBJECT_0) {
          DWORD code = 0;
          GetExitCodeProcess(child.process, &code);
          CloseHandle(child.process);
          if (error) *error = "updater exited early with code " + std::to_string(code) + "\n" + log_tail(log_file, 4);
          return false;
        }
      }
      // §REVIEW-1111：放棄時把 updater 結束掉。原本只關 handle：UAC 對話框還開著，使用者過了這 240 秒才按「是」，
      // 提升後的腳本沒有人在等它，卻照樣強制結束 server、覆蓋檔案、重開。
      TerminateProcess(child.process, 1);
      WaitForSingleObject(child.process, 3000);
      CloseHandle(child.process);
      if (error) *error = "updater did not become ready (UAC pending or failed); see " + log_file.string();
      return false;
    }
#else
    // 內嵌的 Linux 更新腳本（以 root 執行，位於使用者 0700 的 update 目錄）。
    //   §SELF-UPDATE (4)（2026-11-20）：.deb 另外把同一支腳本裝在固定路徑（SUNSHINE_UPDATE_HELPER）；
    //   那一支存在時優先用它，這份內嵌的只當備援（還沒裝過帶 helper 的版本、或不是從 .deb 安裝）。
    //   審查 F7：兩者共用同一份來源 src_assets/linux/misc/self-update-helper.sh——建置時由
    //   cmake/compile_definitions/common.cmake 包成 self_update_helper_script.h，這裡不再手寫腳本內容；
    //   腳本行為（參數格式、複製到 root 目錄再驗再裝、拒絕降版）看那支 .sh 的檔頭。
    //   參數：<套件絕對路徑> <64 字元小寫 sha256> <0|1：同版時是否 --reinstall>；結果檔由呼叫端（使用者身分）寫。
    constexpr const char *kLinuxScript = kViplestreamSelfUpdateHelperScript;

    // fork/execvp（不經 shell、不需引號逃逸），stdout/stderr 導向 log 檔
    int run_argv(const std::vector<std::string> &argv, const std::filesystem::path &log) {
      std::vector<char *> args;
      for (const auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
      args.push_back(nullptr);
      const pid_t pid = fork();
      if (pid < 0) return -1;
      if (pid == 0) {
        const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
          dup2(fd, STDOUT_FILENO);
          dup2(fd, STDERR_FILENO);
          if (fd > STDERR_FILENO) close(fd);
        }
        const int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
          dup2(nullfd, STDIN_FILENO);
          if (nullfd > STDERR_FILENO) close(nullfd);
        }
        execvp(args[0], args.data());
        _exit(127);
      }
      int status = 0;
      if (waitpid(pid, &status, 0) < 0) return -1;
      if (WIFEXITED(status)) return WEXITSTATUS(status);
      return -1;
    }

    /// §SELF-UPDATE (4)：.deb 裝在固定路徑的 helper 可不可以用。要是 root 擁有、其他人不可寫的一般檔案才算——
    /// polkit 的政策檔（app.viplestream.server.self-update）靠這個路徑對上 action，授權對話框才會顯示看得懂的說明；
    /// 也不必再讓 root 執行放在使用者目錄裡的腳本。
    std::string installed_helper() {
#ifdef SUNSHINE_UPDATE_HELPER
      struct stat st {};
      if (lstat(SUNSHINE_UPDATE_HELPER, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == 0 &&
          (st.st_mode & 022) == 0 && (st.st_mode & 0100) != 0) {
        return SUNSHINE_UPDATE_HELPER;
      }
#endif
      return {};
    }

    bool apply_linux(const std::filesystem::path &package, const release_info &rel, std::string *error) {
      const auto dir = update_dir();
      if (dir.empty()) {
        if (error) *error = "update directory is not private (refusing to run installer)";
        return false;
      }
      std::error_code ec;
      const std::string helper = installed_helper();
      const auto script = dir / "self_update.sh";
      if (helper.empty()) {
        std::ofstream out(script, std::ios::binary | std::ios::trunc);
        if (!out) {
          if (error) *error = "cannot write " + script.string();
          return false;
        }
        out << kLinuxScript;
        out.close();
        std::filesystem::permissions(script, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace, ec);
      }
      BOOST_LOG(info) << "[VIPLE-UPDATE] installer: " << (helper.empty() ? "embedded script " + script.string() : "helper " + helper);
      const auto log = platf::appdata() / "self_update.log";
      // 審查 F5：helper 不再接受 "-"（略過驗證）。下載路徑（常駐圖示與 CLI 不帶路徑）拿不到 digest 時
      // 在下載前就拒裝了，走到這裡一定有 digest；只有 CLI `--self-update <本機檔案>` 會清空 digest
      // （來源由下指令的人負責）。那種情況就在這裡算出檔案目前的 sha256 傳下去：helper 複製到 root 目錄後
      // 比對的是「server 看到的那份內容」，驗證之後才被換掉的檔案不會被裝上去。
      std::string sha = rel.asset_sha256;
      if (sha.empty()) {
        sha = sha256_file(package);
        if (sha.size() != 64) {
          if (error) *error = "cannot compute sha256 of " + package.string();
          return false;
        }
        BOOST_LOG(info) << "[VIPLE-UPDATE] local package sha256 " << sha;
      }
      const auto cmp = compare_versions(current_version(), rel.version);
      // 審查 F6：helper 一律拒絕降版（比對 dpkg 已安裝版本），這個旗標只決定「同版時要不要 --reinstall」。
      // 同版重裝的明確情況：`--self-update --force` 在已是最新版時重裝最新版，以及 `--self-update <本機檔案>`
      // （target.version＝目前版本）。`--force` 在目前版本比 latest 新（開發版）時會被 helper 以降版拒絕。
      const std::string reinstall = (cmp && *cmp == 0) ? "1" : "0";

      // helper 要求絕對路徑（審查 F5）；CLI 的本機檔案可能是相對路徑，這裡先轉成絕對路徑
      const auto package_abs = std::filesystem::absolute(package, ec);
      if (ec || package_abs.empty()) {
        if (error) *error = "cannot resolve package path " + package.string();
        return false;
      }

      // helper 直接當成被執行的程式（pkexec 用它的路徑找 polkit action）；備援才是 /bin/sh＋使用者目錄的腳本
      std::vector<std::string> base = helper.empty() ?
                                        std::vector<std::string> {"/bin/sh", script.string(), package_abs.string(), sha, reinstall} :
                                        std::vector<std::string> {helper, package_abs.string(), sha, reinstall};
      int rc = -1;
      // 1) 免密碼 sudo：先探測，不行就不用它（避免把安裝失敗誤判成沒權限）
      if (run_argv({"sudo", "-n", "true"}, log) == 0) {
        std::vector<std::string> argv = {"sudo", "-n"};
        argv.insert(argv.end(), base.begin(), base.end());
        rc = run_argv(argv, log);
        BOOST_LOG(info) << "[VIPLE-UPDATE] sudo path rc=" << rc;
      }
      else {
        // 2) polkit（桌面 session 會跳授權視窗；無 agent 時 rc 126/127）
        std::vector<std::string> argv = {"pkexec"};
        argv.insert(argv.end(), base.begin(), base.end());
        rc = run_argv(argv, log);
        BOOST_LOG(info) << "[VIPLE-UPDATE] pkexec path rc=" << rc;
        if (rc == 126 || rc == 127) {
          if (error) *error = "no permission to install (polkit rc " + std::to_string(rc) + "). Package saved at " + package.string() +
                              " — run: sudo viplestream-server --self-update " + package.string();
          return false;
        }
      }
      if (rc != 0) {
        if (error) *error = "package install failed (rc " + std::to_string(rc) + ")\n" + log_tail(log, 4);
        return false;
      }
      // 只刪自己下載到 update 目錄的套件；使用者指定的路徑不能動
      if (package.parent_path() == dir) {
        std::filesystem::remove(package, ec);
      }
      return true;
    }
#endif

    struct worker_guard_t {
      state_fn st;
      ~worker_guard_t() {
        if (st) st(false, label_for(g_state.load(), pending()));
        g_busy.store(false);
      }
    };

    // 啟動背景執行緒（可 join；前一輪結束後才會開新一輪）
    bool start_worker(std::function<void()> body) {
      bool expected = false;
      if (!g_busy.compare_exchange_strong(expected, true)) return false;
      std::lock_guard<std::mutex> lg(g_worker_mtx);
      if (g_worker.joinable()) g_worker.join();
      try {
        g_worker = std::thread([body = std::move(body)]() {
          try {
            body();
          }
          catch (const std::exception &e) {
            BOOST_LOG(warning) << "[VIPLE-UPDATE] worker exception: " << e.what();
          }
          catch (...) {
            BOOST_LOG(warning) << "[VIPLE-UPDATE] worker unknown exception";
          }
        });
      }
      catch (const std::exception &e) {
        BOOST_LOG(warning) << "[VIPLE-UPDATE] cannot start worker: " << e.what();
        g_busy.store(false);
        return false;
      }
      return true;
    }
  }  // namespace

  std::optional<int> compare_versions(const std::string &a, const std::string &b) {
    std::vector<int> va, vb;
    if (!parse_version_parts(a, va) || !parse_version_parts(b, vb)) return std::nullopt;
    const size_t n = std::max(va.size(), vb.size());
    for (size_t i = 0; i < n; i++) {
      const int x = i < va.size() ? va[i] : 0;
      const int y = i < vb.size() ? vb[i] : 0;
      if (x != y) return x < y ? -1 : 1;
    }
    return 0;
  }

  std::optional<release_info> query_latest(std::string *error) {
    std::string head_err;
    if (auto rel = query_via_head(&head_err)) {
      BOOST_LOG(info) << "[VIPLE-UPDATE] latest via HEAD: " << rel->version;
      return rel;
    }
    BOOST_LOG(warning) << "[VIPLE-UPDATE] HEAD probe failed (" << head_err << "), falling back to API";
    std::string api_err;
    if (auto rel = query_via_api(&api_err)) {
      BOOST_LOG(info) << "[VIPLE-UPDATE] latest via API: " << rel->version;
      return rel;
    }
    if (error) *error = head_err + "; " + api_err;
    return std::nullopt;
  }

  bool fetch_asset_details(release_info &rel, std::string *error) {
    long code = 0;
    auto j = api_get_json(std::string(kApiBase) + "/tags/v" + rel.version, error, &code);
    if (!j) return false;
    remember_notes(rel.version, *j);  // §UPDATE-NOTES-TRAY：同一份 JSON 帶著內文
    auto assets = j->find("assets");
    if (assets == j->end() || !assets->is_array()) {
      if (error) *error = "release JSON has no assets[]";
      return false;
    }
    for (const auto &a : *assets) {
      if (!a.is_object() || json_str(a, "name") != rel.asset_name) continue;
      rel.asset_listed = true;
      const std::string url = json_str(a, "browser_download_url");
      if (!url.empty()) rel.asset_url = url;
      auto sz = a.find("size");
      if (sz != a.end() && sz->is_number_integer()) rel.asset_size = sz->get<long long>();
      std::string digest = json_str(a, "digest");  // "sha256:<hex>"
      if (digest.rfind("sha256:", 0) == 0) {
        digest.erase(0, 7);
        std::transform(digest.begin(), digest.end(), digest.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        if (digest.size() == 64 && digest.find_first_not_of("0123456789abcdef") == std::string::npos) {
          rel.asset_sha256 = digest;
        }
      }
      return true;
    }
    rel.asset_listed = false;
    return true;
  }

  std::string sha256_file(const std::filesystem::path &file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return {};
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    std::vector<char> buf(1 << 16);
    while (in) {
      in.read(buf.data(), (std::streamsize) buf.size());
      const auto n = in.gcount();
      if (n > 0) EVP_DigestUpdate(ctx, buf.data(), (size_t) n);
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx, md, &len);
    EVP_MD_CTX_free(ctx);
    static const char *hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < len; i++) {
      out.push_back(hex[md[i] >> 4]);
      out.push_back(hex[md[i] & 0xF]);
    }
    return out;
  }

  bool download_file(const std::string &url, const std::filesystem::path &dest, std::string *error) {
    CURL *curl = curl_easy_init();  // NOSONAR
    if (!curl) {
      if (error) *error = "curl init failed";
      return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(dest.parent_path(), ec);
#ifdef _WIN32
    FILE *fp = _wfopen(dest.c_str(), L"wb");
#else
    FILE *fp = fopen(dest.c_str(), "wb");
#endif
    if (!fp) {
      curl_easy_cleanup(curl);
      if (error) *error = "cannot open " + dest.string();
      return false;
    }
    curl_common(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
#ifndef _WIN32
    // 審查 F8：下載（含重新導向）只走 https；GitHub 的 asset 一律是 https 轉到 objects.githubusercontent.com。
  #if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0 起用字串版，舊的 bitmask 版已標為 deprecated
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  #else
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, (long) CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, (long) CURLPROTO_HTTPS);
  #endif
#endif
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    const CURLcode rc = curl_easy_perform(curl);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    const std::string err = curl_error_text(rc, code);
    curl_easy_cleanup(curl);
    fclose(fp);
    if (rc != CURLE_OK) {
      std::filesystem::remove(dest, ec);
      if (error) *error = "download " + url + ": " + err;
      return false;
    }
    if (!looks_like_package(dest)) {
      std::filesystem::remove(dest, ec);
      if (error) *error = "downloaded file is not a valid package: " + dest.string();
      return false;
    }
    BOOST_LOG(info) << "[VIPLE-UPDATE] downloaded " << url << " → " << dest.string()
                    << " (" << std::filesystem::file_size(dest, ec) << " bytes)";
    return true;
  }

  bool apply_package(const std::filesystem::path &package, const release_info &rel, bool tray_mode, std::string *error) {
    if (!looks_like_package(package)) {
      if (error) *error = "not a valid package: " + package.string();
      return false;
    }
#ifdef _WIN32
    return apply_windows(package, rel, tray_mode, error);
#else
    (void) tray_mode;
    return apply_linux(package, rel, error);
#endif
  }

  state_e state() {
    return g_state.load();
  }

  std::optional<release_info> pending() {
    std::lock_guard<std::mutex> lg(g_pending_mtx);
    return g_pending;
  }

  std::optional<std::string> take_last_result() {
    const auto path = result_path();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return std::nullopt;
    std::string text;
    {
      std::ifstream in(path);
      std::getline(in, text);
    }
    if (!std::filesystem::remove(path, ec)) {
      // 刪不掉（權限）就清空，避免每次啟動都重複通知
      std::ofstream(path, std::ios::trunc);
    }
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (text.empty()) return std::nullopt;
    return text;
  }

  bool check_async(notify_fn notify, state_fn on_state) {
    return start_worker([notify, on_state]() {
      worker_guard_t guard {on_state};
      set_state(state_e::checking, on_state);
      const auto cur = current_version();
      std::string err;
      auto latest = query_latest(&err);
      if (!latest) {
        set_state(state_e::idle, on_state);
        if (notify) notify("Update check failed", "Could not reach GitHub: " + err);
        return;
      }
      const auto cmp = compare_versions(cur, latest->version);
      if (!cmp || *cmp >= 0) {
        BOOST_LOG(info) << "[VIPLE-UPDATE] up to date: current " << cur << ", latest " << latest->version;
        {
          std::lock_guard<std::mutex> lg(g_pending_mtx);
          g_pending.reset();
        }
        set_state(state_e::idle, on_state);
        if (notify) notify("VipleStream is up to date", "Version " + cur + " is the latest release.");
        return;
      }
      BOOST_LOG(info) << "[VIPLE-UPDATE] update available: " << cur << " → " << latest->version;
      // §UPDATE-NOTES-TRAY：先拿更新摘要（一版只查一次；拿不到就照舊只報版號），再把選單換成「Install update」。
      // 2026-11-20 第三輪審查：原本先改選單再查（最多 10 秒），這段期間 worker 還佔著，按「Install update」會被擋下來
      // 顯示「already in progress」。現在查摘要時狀態還是 checking（選單停用），選單變成可按時 worker 只剩發通知
      latest->summary = notes_summary_for(latest->version);
      {
        std::lock_guard<std::mutex> lg(g_pending_mtx);
        g_pending = latest;
      }
      set_state(state_e::update_available, on_state);
      if (notify) notify("VipleStream update available", with_summary("Version " + latest->version + " is available. Click \"Install update v" + latest->version + "\" in the tray menu to install (streaming will be interrupted).", latest->summary));
    });
  }

  bool install_pending_async(notify_fn notify, state_fn on_state, std::string *why_not) {
    std::optional<release_info> rel;
    {
      std::lock_guard<std::mutex> lg(g_pending_mtx);
      rel = g_pending;
    }
    if (!rel) {
      if (why_not) *why_not = "No update is pending. Check for updates first.";
      return false;
    }
    if (rtsp_stream::session_count() > 0) {
      if (why_not) *why_not = "A stream is active. Stop streaming before installing the update.";
      return false;
    }
    auto target = *rel;  // 非 const：by-value 捕捉會保留 const，mutable lambda 內也改不了
    const bool started = start_worker([notify, on_state, target]() mutable {
      worker_guard_t guard {on_state};
      std::string err;
      set_state(state_e::downloading, on_state);
      // 安裝前一定要拿到 digest（fail closed）
      if (!fetch_asset_details(target, &err)) {
        set_state(state_e::update_available, on_state);
        if (notify) notify("Update failed", "Cannot verify package integrity: " + err);
        return;
      }
      if (!target.asset_listed) {
        set_state(state_e::idle, on_state);
        {
          std::lock_guard<std::mutex> lg(g_pending_mtx);
          g_pending.reset();
        }
        if (notify) notify("No package for this platform", "Release v" + target.version + " has no " + target.asset_name + ". Nothing to install.");
        return;
      }
      if (target.asset_sha256.empty()) {
        set_state(state_e::update_available, on_state);
        if (notify) notify("Update failed", "Release asset has no sha256 digest; refusing to install unverified package.");
        return;
      }
      const auto dir = update_dir();
      if (dir.empty()) {
        set_state(state_e::update_available, on_state);
        if (notify) notify("Update failed", "No safe update directory available.");
        return;
      }
      const auto dest = dir / target.asset_name;
      if (notify) notify("Downloading update", "Version " + target.version + " (" + target.asset_name + ")...");
      if (!download_file(target.asset_url, dest, &err)) {
        set_state(state_e::update_available, on_state);
        if (notify) notify("Update download failed", err);
        return;
      }
      const auto sum = sha256_file(dest);
      if (sum != target.asset_sha256) {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        set_state(state_e::update_available, on_state);
        BOOST_LOG(warning) << "[VIPLE-UPDATE] sha256 mismatch: got " << sum << " expected " << target.asset_sha256;
        if (notify) notify("Update failed", "Package checksum mismatch; download discarded.");
        return;
      }
      BOOST_LOG(info) << "[VIPLE-UPDATE] sha256 verified " << sum;
      // §REVIEW-1111：「串流中不安裝」原本只在開始下載前看一次；下載與驗證期間開始的串流會被停服務打斷。
      if (rtsp_stream::session_count() > 0) {
        std::error_code ec;
        std::filesystem::remove(dest, ec);
        g_deferred_by_stream.store(true);
        set_state(state_e::update_available, on_state);
        BOOST_LOG(info) << "[VIPLE-UPDATE] a stream started during the download; install postponed";
        if (notify) notify("Update postponed", "A stream started while downloading. The update will be installed later.");
        return;
      }
      set_state(state_e::installing, on_state);
      if (notify) notify("Installing update", "Version " + target.version + ". The server will restart shortly.");
      if (!apply_package(dest, target, true, &err)) {
        set_state(state_e::update_available, on_state);
        BOOST_LOG(warning) << "[VIPLE-UPDATE] " << err;
        if (notify) notify("Update failed", err);
        return;
      }
#ifndef _WIN32
      // Linux：套件已裝好。systemd user unit（Restart=always）下直接退出交給 systemd；
      // 否則 platf::restart()（linux/misc.cpp 已處理 /proc/self/exe 的 " (deleted)" 尾綴）。
      write_result(target.version);
      {
        std::lock_guard<std::mutex> lg(g_pending_mtx);
        g_pending.reset();
      }
      set_state(state_e::idle, on_state);
      BOOST_LOG(info) << "[VIPLE-UPDATE] package installed, restarting";
      if (std::getenv("INVOCATION_ID")) {
        lifetime::exit_sunshine(0, true);
      }
      else {
        platf::restart();
      }
#endif
    });
    if (!started && why_not) {
      // 還有一個 worker 在跑。查版剛做完、只剩發通知時（state 已是 update_available）提示稍後再按；下載／安裝中維持原本的說法
      *why_not = g_state.load() == state_e::update_available ? "Still finishing the update check. Try again in a moment." :
                                                               "An update is already in progress.";
    }
    return started;
  }

  // ── §AUTO-UPDATE ──
  namespace {
    std::mutex g_auto_mtx;
    std::condition_variable g_auto_cv;
    std::thread g_auto;
    bool g_auto_stop = false;
    std::string g_auto_mode;
    std::string g_auto_announced;  ///< 已經跳過「有新版」通知的版號（每版只跳一次）
    std::string g_auto_tried;  ///< 已經自動安裝過的版號（失敗不重試）
    std::mutex g_ui_mtx;
    notify_fn g_ui_notify;
    state_fn g_ui_state;

    notify_fn ui_notify() {
      std::lock_guard<std::mutex> lg(g_ui_mtx);
      if (g_ui_notify) return g_ui_notify;
      return [](const std::string &title, const std::string &text) {
        BOOST_LOG(info) << "[VIPLE-UPDATE] (no tray) " << title << ": " << text;
      };
    }

    state_fn ui_state() {
      std::lock_guard<std::mutex> lg(g_ui_mtx);
      return g_ui_state;
    }

    /// 自動檢查的通知過濾：只轉送「有新版」，而且每版一次；「已是最新」「查詢失敗」只留 log
    void auto_notify(const std::string &title, const std::string &text) {
      if (title != "VipleStream update available") {
        BOOST_LOG(info) << "[VIPLE-UPDATE] auto check: " << title << " (" << text << ")";
        return;
      }
      const auto pend = pending();
      std::string mode;
      {
        std::lock_guard<std::mutex> lg(g_auto_mtx);
        if (!pend || g_auto_announced == pend->version) return;
        g_auto_announced = pend->version;
        mode = g_auto_mode;
      }
      if (mode == "install") {
        ui_notify()(title, with_summary("Version " + pend->version + " will be installed automatically when nobody is streaming. "
                                        "You can also install it now from the tray menu.", pend->summary));
      } else {
        ui_notify()(title, text);
      }
    }

    /// 等 ms 毫秒或被 shutdown 叫醒；回 true＝要停止
    bool auto_wait(std::chrono::milliseconds ms) {
      std::unique_lock<std::mutex> lk(g_auto_mtx);
      return g_auto_cv.wait_for(lk, ms, [] { return g_auto_stop; });
    }

    void auto_loop() {
      using namespace std::chrono;
      if (auto_wait(minutes(3))) return;
      std::string mode;
      {
        std::lock_guard<std::mutex> lg(g_auto_mtx);
        mode = g_auto_mode;
      }
      auto last_stream = steady_clock::now();
      while (true) {
        const auto st = state();
        if (st == state_e::idle || st == state_e::update_available) {
          BOOST_LOG(info) << "[VIPLE-UPDATE] auto check (update_mode=" << mode << ")";
          check_async(auto_notify, ui_state());
        }
        // 下一次檢查：24 小時後（加最多 1 小時的隨機偏移，避免很多台同時打 GitHub）
        const auto next = steady_clock::now() + hours(24) + minutes(std::rand() % 60);
        while (steady_clock::now() < next) {
          if (auto_wait(minutes(1))) return;
          if (mode != "install") continue;
          if (g_deferred_by_stream.exchange(false)) {
            // 上一次是被串流擋下來的，不算試過
            std::lock_guard<std::mutex> lg(g_auto_mtx);
            g_auto_tried.clear();
            last_stream = steady_clock::now();
          }
          if (rtsp_stream::session_count() > 0) {
            last_stream = steady_clock::now();
            continue;
          }
          const auto pend = pending();
          if (!pend || state() != state_e::update_available || steady_clock::now() - last_stream < minutes(10)) continue;
          {
            std::lock_guard<std::mutex> lg(g_auto_mtx);
            if (g_auto_tried == pend->version) continue;
          }
          BOOST_LOG(info) << "[VIPLE-UPDATE] auto install v" << pend->version << " (no stream for 10 min)";
          std::string why;
          if (install_pending_async(ui_notify(), ui_state(), &why)) {
            // 開始了就記住：這一版裝失敗不再自動重試（失敗通知已由安裝流程跳出），等下一版或手動
            std::lock_guard<std::mutex> lg(g_auto_mtx);
            g_auto_tried = pend->version;
          } else {
            BOOST_LOG(info) << "[VIPLE-UPDATE] auto install not started: " << why;
          }
        }
      }
    }
  }  // namespace

  void set_ui(notify_fn notify, state_fn on_state) {
    std::lock_guard<std::mutex> lg(g_ui_mtx);
    g_ui_notify = std::move(notify);
    g_ui_state = std::move(on_state);
  }

  void start_auto(const std::string &mode) {
    if (mode == "off") {
      BOOST_LOG(info) << "[VIPLE-UPDATE] automatic update checks are off (update_mode=off)";
      return;
    }
    std::lock_guard<std::mutex> lg(g_auto_mtx);
    if (g_auto.joinable()) return;
    g_auto_mode = mode == "install" ? "install" : "notify";
    g_auto_stop = false;
    try {
      g_auto = std::thread(auto_loop);
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "[VIPLE-UPDATE] cannot start auto update thread: " << e.what();
    }
  }

  void shutdown() {
    {
      std::lock_guard<std::mutex> lg(g_auto_mtx);
      g_auto_stop = true;
    }
    g_auto_cv.notify_all();
    if (g_auto.joinable()) {
      if (g_auto.get_id() == std::this_thread::get_id()) {
        g_auto.detach();
      } else {
        g_auto.join();
      }
    }
    g_cancel.store(true);
    std::lock_guard<std::mutex> lg(g_worker_mtx);
    if (!g_worker.joinable()) return;
    // §REVIEW-1103：更新完成後是在 g_worker 自己身上 exit_sunshine → raise(SIGINT)，處理常式
    // 同步跑到這裡；join 自己會丟 EDEADLK，還會跳過後面的 tray 收尾與顯示器設定還原。
    // 是自己就 detach（行程馬上要結束），讓 SIGINT 處理常式照常做完收尾。
    if (g_worker.get_id() == std::this_thread::get_id()) {
      g_worker.detach();
      return;
    }
    g_worker.join();
  }

  int cli_check_update(int, char **) {
    std::string err;
    const auto cur = current_version();
    auto latest = query_latest(&err);
    if (!latest) {
      std::cout << "current " << cur << "; check failed: " << err << std::endl;
      return 1;
    }
    const auto cmp = compare_versions(cur, latest->version);
    std::cout << "current " << cur << "; latest " << latest->version << " (" << latest->page_url << ")" << std::endl;
    if (cmp && *cmp < 0) {
      std::cout << "update available: " << latest->asset_url << std::endl;
      return 2;
    }
    std::cout << "up to date" << std::endl;
    return 0;
  }

  int cli_self_update(int argc, char **argv) {
    bool force = false;
    std::filesystem::path package;
    for (int i = 0; i < argc; i++) {
      const std::string_view a = argv[i];
      if (a == "--force"sv) force = true;
      else if (!a.empty() && a[0] != '-') package = std::string(a);
    }
    const auto cur = current_version();
    std::string err;
    release_info target = make_release(cur);
    if (package.empty()) {
      auto latest = query_latest(&err);
      if (!latest) {
        std::cout << "check failed: " << err << std::endl;
        return 1;
      }
      const auto cmp = compare_versions(cur, latest->version);
      std::cout << "current " << cur << "; latest " << latest->version << std::endl;
      if (!force && (!cmp || *cmp >= 0)) {
        std::cout << "up to date (use --force to reinstall latest)" << std::endl;
        return 0;
      }
      target = *latest;
      if (!fetch_asset_details(target, &err)) {
        std::cout << "cannot verify package integrity: " << err << std::endl;
        return 1;
      }
      if (!target.asset_listed) {
        std::cout << "release v" << target.version << " has no " << target.asset_name << " for this platform" << std::endl;
        return 1;
      }
      if (target.asset_sha256.empty()) {
        std::cout << "release asset has no sha256 digest; refusing to install unverified package" << std::endl;
        return 1;
      }
      const auto dir = update_dir();
      if (dir.empty()) {
        std::cout << "no safe update directory" << std::endl;
        return 1;
      }
      package = dir / target.asset_name;
      std::cout << "downloading " << target.asset_url << std::endl;
      if (!download_file(target.asset_url, package, &err)) {
        std::cout << "download failed: " << err << std::endl;
        return 1;
      }
      const auto sum = sha256_file(package);
      if (sum != target.asset_sha256) {
        std::cout << "sha256 mismatch: got " << sum << " expected " << target.asset_sha256 << std::endl;
        return 1;
      }
      std::cout << "sha256 verified" << std::endl;
    }
    else {
      // 本機套件：不驗 digest（來源由使用者負責），但腳本仍驗套件形式
      target.asset_sha256.clear();
    }
    std::cout << "applying " << package.string() << " (version " << target.version << ")" << std::endl;
    if (!apply_package(package, target, false, &err)) {
      std::cout << "apply failed: " << err << std::endl;
      return 1;
    }
#ifdef _WIN32
    std::cout << "update applied; the service/server has been restarted" << std::endl;
#else
    write_result(target.version);
    std::cout << "package installed; restart the server to run the new version" << std::endl;
#endif
    return 0;
  }

}  // namespace self_update
