/**
 * @file src/vr/vr_padmode.cpp
 * @brief §VR-PADMODE：手把模式的決策、Steam 商店分類查詢與手動選擇的持久化。
 */
#include "vr_padmode.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "src/logging.h"
#include "src/platform/common.h"

#ifdef _WIN32
  #include <windows.h>

  #include "src/platform/windows/steam_scanner.h"
#endif

using namespace std::literals;

namespace vr::padmode {

  static_assert(k_dpad_up == platf::DPAD_UP && k_dpad_down == platf::DPAD_DOWN && k_dpad_left == platf::DPAD_LEFT &&
                  k_dpad_right == platf::DPAD_RIGHT && k_start == platf::START && k_back == platf::BACK &&
                  k_left_stick == platf::LEFT_STICK && k_right_stick == platf::RIGHT_STICK && k_left_button == platf::LEFT_BUTTON &&
                  k_right_button == platf::RIGHT_BUTTON && k_a == platf::A && k_b == platf::B && k_x == platf::X && k_y == platf::Y,
                "vr_padmode_map.h 的按鍵旗標必須與 platf::* 同值");

  // ── controller_t ──

  controller_t::controller_t(hooks_t hooks, bool start_thread):
      h_(std::move(hooks)) {
    if (start_thread) {
      worker_ = std::thread([this]() {
        std::unique_lock lk(mtx_);
        while (!stop_) {
          lk.unlock();
          poll_once();
          lk.lock();
          cv_.wait_for(lk, std::chrono::milliseconds(k_poll_ms), [this]() {
            return stop_ || toggle_req_;
          });
        }
      });
    }
  }

  controller_t::~controller_t() {
    {
      std::lock_guard lk(mtx_);
      stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    std::lock_guard lk(mtx_);
    if (pad_id_ >= 0 && h_.pad_free) {
      h_.pad_free(pad_id_);
      BOOST_LOG(info) << "[VIPLE-VR-PADMODE] session ended, virtual Xbox pad removed";
    }
    pad_id_ = -1;
  }

  bool controller_t::pad_active() const {
    std::lock_guard lk(mtx_);
    return pad_id_ >= 0;
  }

  void controller_t::on_sample(const VIPLE_VR_CONTROLLER_INPUT in[2], int64_t now_ms) {
    bool wake = false;
    {
      std::lock_guard lk(mtx_);
      bool held = false;
      if (combo_.step(both_menu(in), now_ms, k_hold_ms, &held)) {
        toggle_req_ = true;
        wake = true;
      }
      if (pad_id_ >= 0 && h_.pad_update) {
        const xpad_t x = map_frame(in, held);
        if (!sent_ || !(x == last_)) {
          h_.pad_update(pad_id_, x);
          last_ = x;
          sent_ = true;
        }
      }
    }
    if (wake) {
      cv_.notify_all();
    }
  }

  void controller_t::poll_once() {
    bool toggle = false;
    bool active = false;
    {
      std::lock_guard lk(mtx_);
      toggle = toggle_req_;
      toggle_req_ = false;
      active = pad_id_ >= 0;
    }

    const uint32_t pid = h_.scene_pid ? h_.scene_pid() : 0;
    if (pid != pid_) {
      pid_ = pid;
      app_ = pid != 0 && h_.resolve ? h_.resolve(pid) : std::nullopt;
      if (app_) {
        BOOST_LOG(info) << "[VIPLE-VR-PADMODE] scene app pid=" << pid << " key=" << app_->key << " name=\"" << app_->name << '"';
      } else {
        BOOST_LOG(info) << "[VIPLE-VR-PADMODE] scene app pid=" << pid << (pid ? " (not a known game)" : " (none)");
      }
      decided_ = false;
    }

    if (toggle) {
      const bool want = !active;
      if (app_ && !app_->key.empty()) {
        if (h_.set_override) {
          h_.set_override(app_->key, want);
        }
      } else {
        session_override_set_ = true;
        session_override_ = want;
      }
      BOOST_LOG(info) << "[VIPLE-VR-PADMODE] manual toggle -> " << (want ? "xbox pad" : "vr controllers") << " for "
                      << (app_ ? app_->key + " (" + app_->name + ")" : std::string("no scene app (this session only)"));
      decided_ = true;
      apply(want, source_e::manual);
      return;
    }
    if (decided_) {
      return;
    }

    if (!app_ || app_->key.empty()) {
      decided_ = true;
      if (session_override_set_) {
        apply(session_override_, source_e::none);
      } else {
        apply(false, source_e::none);
      }
      return;
    }
    if (h_.get_override) {
      if (const auto o = h_.get_override(app_->key)) {
        decided_ = true;
        BOOST_LOG(info) << "[VIPLE-VR-PADMODE] " << app_->key << ": remembered manual choice -> " << (*o ? "xbox pad" : "vr controllers");
        // 記住的選擇：開成手把模式時提示（跟商店判斷一樣），維持 VR 手把不提示
        apply(*o, source_e::store);
        return;
      }
    }
    if (app_->appid.empty() || !h_.tracked) {
      decided_ = true;
      apply(false, source_e::none);
      return;
    }
    const auto t = h_.tracked(app_->appid);
    if (!t) {
      // 商店結果還沒回來：先維持 VR 手把，下一輪再看
      apply(false, source_e::none);
      return;
    }
    decided_ = true;
    BOOST_LOG(info) << "[VIPLE-VR-PADMODE] " << app_->appid << ": store says tracked controllers "
                    << (*t ? "supported -> vr controllers" : "NOT supported -> xbox pad");
    apply(!*t, source_e::store);
  }

  void controller_t::apply(bool want_pad, source_e source) {
    int cur;
    {
      std::lock_guard lk(mtx_);
      cur = pad_id_;
    }
    if (want_pad && cur < 0) {
      // 建立可能要幾十～上百 ms（ViGEm），不持鎖
      const int id = h_.pad_alloc ? h_.pad_alloc() : -1;
      if (id < 0) {
        BOOST_LOG(warning) << "[VIPLE-VR-PADMODE] could not create the virtual Xbox pad (ViGEmBus missing?); staying on vr controllers";
        if (!unavailable_noticed_ && h_.notice) {
          unavailable_noticed_ = true;
          h_.notice(notice_e::pad_unavailable);
        }
        return;
      }
      {
        std::lock_guard lk(mtx_);
        pad_id_ = id;
        sent_ = false;
      }
      BOOST_LOG(info) << "[VIPLE-VR-PADMODE] xbox pad mode ON (" << (source == source_e::manual ? "manual" : "auto") << ") pad=" << id;
      if (h_.notice) {
        h_.notice(source == source_e::manual ? notice_e::pad_on_manual : notice_e::pad_on_auto);
      }
    } else if (!want_pad && cur >= 0) {
      {
        std::lock_guard lk(mtx_);
        pad_id_ = -1;
        sent_ = false;
      }
      if (h_.pad_free) {
        h_.pad_free(cur);
      }
      BOOST_LOG(info) << "[VIPLE-VR-PADMODE] xbox pad mode OFF (" << (source == source_e::manual ? "manual" : "auto") << ")";
      // 自動關（遊戲結束、換成 VR 手把遊戲）不提示，只有手動切回才提示
      if (source == source_e::manual && h_.notice) {
        h_.notice(notice_e::pad_off_manual);
      }
    } else if (source == source_e::manual && h_.notice) {
      // 手動要求的狀態本來就是現在的狀態，仍然回覆一次讓使用者知道
      h_.notice(want_pad ? notice_e::pad_on_manual : notice_e::pad_off_manual);
    }
  }

  // ── 持久化：appdata/vr_padmode.json ──
  // {"store": {"<appid>": {"tracked": true|false, "t": <unix 秒>}}, "manual": {"<key>": true|false}}

  namespace {
    std::mutex g_mtx;
    bool g_loaded = false;
    nlohmann::json g_db = nlohmann::json::object();
    std::set<std::string> g_inflight;
    std::map<std::string, int64_t> g_retry_after;  ///< appid → 查詢失敗後，下一次可以重查的時間（unix 秒）

    int64_t unix_now() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::filesystem::path db_path() {
      return platf::appdata() / "vr_padmode.json";
    }

    void load_locked() {
      if (g_loaded) {
        return;
      }
      g_loaded = true;
      std::ifstream f(db_path());
      if (!f) {
        return;
      }
      auto j = nlohmann::json::parse(f, nullptr, false);
      if (!j.is_discarded() && j.is_object()) {
        g_db = std::move(j);
      } else {
        BOOST_LOG(warning) << "[VIPLE-VR-PADMODE] vr_padmode.json is malformed, starting over";
      }
    }

    /// 改之前重讀一次：使用者（或別的工具）手動改過檔案時，以檔案為準再套用這次的變更，不會被記憶體裡的舊內容蓋掉
    void reload_locked() {
      g_loaded = false;
      g_db = nlohmann::json::object();
      load_locked();
    }

    void save_locked() {
      const auto p = db_path();
      const auto tmp = p.string() + ".tmp";
      {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
          BOOST_LOG(warning) << "[VIPLE-VR-PADMODE] cannot write " << tmp;
          return;
        }
        f << g_db.dump(2);
      }
      std::error_code ec;
      std::filesystem::rename(tmp, p, ec);
      if (ec) {
        BOOST_LOG(warning) << "[VIPLE-VR-PADMODE] cannot replace " << p.string() << ": " << ec.message();
      }
    }

    size_t write_cb(char *ptr, size_t size, size_t nmemb, void *ud) {
      auto *s = static_cast<std::string *>(ud);
      const size_t n = size * nmemb;
      if (s->size() + n > 1024 * 1024) {
        return 0;  // 商店回應不該超過 1 MB
      }
      s->append(ptr, n);
      return n;
    }

    /// @return true／false＝商店有沒有 Tracked Controller Support；nullopt＝查詢失敗（稍後重試）
    std::optional<bool> fetch_store(const std::string &appid, std::string *why) {
      CURL *curl = curl_easy_init();  // NOSONAR
      if (!curl) {
        *why = "curl init failed";
        return std::nullopt;
      }
      std::string body;
      const std::string url = "https://store.steampowered.com/api/appdetails?appids=" + appid + "&filters=categories";
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "VipleStream-Server");
      curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#ifdef _WIN32
      curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long) CURLSSLOPT_NATIVE_CA);
#endif
      const CURLcode rc = curl_easy_perform(curl);
      long code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
      curl_easy_cleanup(curl);
      if (rc != CURLE_OK || code != 200) {
        *why = "HTTP " + std::to_string(code) + " curl=" + curl_easy_strerror(rc);
        return std::nullopt;
      }
      const auto j = nlohmann::json::parse(body, nullptr, false);
      if (j.is_discarded() || !j.is_object() || !j.contains(appid)) {
        *why = "malformed JSON";
        return std::nullopt;
      }
      const auto &e = j[appid];
      if (!e.is_object() || !e.value("success", false)) {
        // 商店沒有這個 app（下架、地區限制）：不知道就維持 VR 手把，當成支援
        *why = "not in store";
        return true;
      }
      const auto data = e.find("data");
      if (data == e.end() || !data->is_object()) {
        *why = "no categories";
        return true;
      }
      const auto cats = data->find("categories");
      if (cats == data->end() || !cats->is_array()) {
        *why = "no categories";
        return true;
      }
      for (const auto &c : *cats) {
        if (c.is_object() && c.value("id", 0) == 52) {
          return true;
        }
      }
      *why = "store has no category 52";
      return false;
    }

    void start_fetch_locked(const std::string &appid) {
      if (g_inflight.count(appid)) {
        return;
      }
      if (const auto it = g_retry_after.find(appid); it != g_retry_after.end() && unix_now() < it->second) {
        return;
      }
      g_inflight.insert(appid);
      std::thread([appid]() {
        std::string why;
        const auto r = fetch_store(appid, &why);
        std::lock_guard lk(g_mtx);
        g_inflight.erase(appid);
        if (!r) {
          g_retry_after[appid] = unix_now() + 600;
          BOOST_LOG(warning) << "[VIPLE-VR-PADMODE] store lookup for " << appid << " failed (" << why << "), retry in 10 min";
          return;
        }
        BOOST_LOG(info) << "[VIPLE-VR-PADMODE] store lookup " << appid << ": tracked=" << (*r ? "yes" : "no")
                        << (why.empty() ? "" : " (" + why + ")");
        reload_locked();
        g_db["store"][appid] = {{"tracked", *r}, {"t", unix_now()}};
        save_locked();
      }).detach();
    }

    bool numeric(const std::string &s) {
      return !s.empty() && s.size() <= 10 && std::all_of(s.begin(), s.end(), [](char c) {
        return c >= '0' && c <= '9';
      });
    }
  }  // namespace

  std::optional<bool> tracked_controller(const std::string &appid) {
    if (!numeric(appid)) {
      return true;
    }
    std::lock_guard lk(g_mtx);
    load_locked();
    if (g_db.contains("store") && g_db["store"].is_object() && g_db["store"].contains(appid)) {
      const auto &e = g_db["store"][appid];
      if (e.is_object() && e.contains("tracked") && e["tracked"].is_boolean()) {
        return e["tracked"].get<bool>();
      }
    }
    start_fetch_locked(appid);
    return std::nullopt;
  }

  std::optional<bool> manual_override(const std::string &key) {
    std::lock_guard lk(g_mtx);
    load_locked();
    if (g_db.contains("manual") && g_db["manual"].is_object() && g_db["manual"].contains(key) && g_db["manual"][key].is_boolean()) {
      return g_db["manual"][key].get<bool>();
    }
    return std::nullopt;
  }

  void set_manual_override(const std::string &key, bool pad) {
    std::lock_guard lk(g_mtx);
    reload_locked();
    g_db["manual"][key] = pad;
    save_locked();
  }

  // ── PID → 遊戲 ──

#ifdef _WIN32
  namespace {
    std::wstring lower(std::wstring s) {
      std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(::towlower(c));
      });
      return s;
    }

    std::string narrow(const std::wstring &w) {
      if (w.empty()) {
        return {};
      }
      const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), nullptr, 0, nullptr, nullptr);
      std::string s(n > 0 ? n : 0, '\0');
      if (n > 0) {
        WideCharToMultiByte(CP_UTF8, 0, w.data(), (int) w.size(), s.data(), n, nullptr, nullptr);
      }
      return s;
    }

    std::wstring widen(const std::string &s) {
      if (s.empty()) {
        return {};
      }
      const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), nullptr, 0);
      std::wstring w(n > 0 ? n : 0, L'\0');
      if (n > 0) {
        MultiByteToWideChar(CP_UTF8, 0, s.data(), (int) s.size(), w.data(), n);
      }
      return w;
    }

    std::wstring normalize_dir(std::wstring d) {
      std::replace(d.begin(), d.end(), L'/', L'\\');
      while (!d.empty() && d.back() == L'\\') {
        d.pop_back();
      }
      return lower(d) + L"\\";
    }
  }  // namespace

  std::optional<app_t> resolve_pid(uint32_t pid) {
    if (pid == 0) {
      return std::nullopt;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
      BOOST_LOG(info) << "[VIPLE-VR-PADMODE] cannot open scene process " << pid << " (error " << GetLastError() << ')';
      return std::nullopt;
    }
    wchar_t buf[MAX_PATH * 2] {};
    DWORD n = (DWORD) std::size(buf);
    const BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &n);
    CloseHandle(h);
    if (!ok || n == 0) {
      return std::nullopt;
    }
    const std::wstring path(buf, n);
    std::wstring lpath = lower(path);
    std::replace(lpath.begin(), lpath.end(), L'/', L'\\');

    if (const auto scan = viple::steam::scan_cached()) {
      const viple::steam::App *best = nullptr;
      size_t best_len = 0;
      for (const auto &a : scan->apps) {
        if (a.install_dir.empty()) {
          continue;
        }
        const std::wstring dir = normalize_dir(widen(a.install_dir));
        if (lpath.size() > dir.size() && lpath.compare(0, dir.size(), dir) == 0 && dir.size() > best_len) {
          best = &a;
          best_len = dir.size();
        }
      }
      if (best) {
        return app_t {best->app_id, best->app_id, best->name};
      }
    }
    const auto slash = path.find_last_of(L"\\/");
    const std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
    return app_t {"exe:" + narrow(lower(file)), "", narrow(file)};
  }
#else
  std::optional<app_t> resolve_pid(uint32_t) {
    return std::nullopt;
  }
#endif

}  // namespace vr::padmode
