/**
 * @file src/vr/vr_selftest.cpp
 * @brief VipleStream §VR（M1b S1-18）：`--vr-selftest` 的平台中立協調器（介面與分工見 vr_selftest.h）。
 */
// standard includes
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <set>
#include <thread>
#include <utility>

// lib includes
#include <curl/curl.h>
#include <nlohmann/json.hpp>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/rtsp.h"
#include "src/vr/vr_clock.h"
#include "src/vr/vr_selftest.h"
#include "src/vr/vr_session.h"

using namespace std::literals;

namespace vr::selftest {
  namespace {
    constexpr std::string_view k_prefix = "[VIPLE-VR-SELFTEST] "sv;
    constexpr std::string_view k_probe_prefix = "[VIPLE-VR-PROBE] "sv;
    constexpr std::size_t k_probe_line_max = 240;

    /// VR 預約的 owner：client 的 UUID 是十六進位 GUID，不會與它相撞
    const std::string k_reservation_owner = "selftest";
    /// 預約的有效期；keeper 每秒續期一次，server 當掉時最多 5 s 後自動失效
    constexpr auto k_reservation_ttl = 5s;
    /// 一次 selftest 的整體上限（§F.5：--detach 的長時間執行也不超過 3 h）
    constexpr auto k_overall_limit = 3h;

    std::atomic<bool> g_running {false};

    /// 子測試的固定執行順序；`implemented_in` 是還沒實作時回給 CLI 的切片
    struct test_def_t {
      std::string_view name;
      std::string_view implemented_in;  ///< 空字串＝V2 已實作
    };

    constexpr test_def_t k_tests[] = {
      {"T0"sv, ""sv},
      {"T0-SECFS"sv, "V5"sv},
      {"T1"sv, ""sv},
      {"T1B"sv, ""sv},
      {"T2"sv, ""sv},
      {"T3"sv, ""sv},  // V5：SteamVR 週期（編排器）
      {"T4"sv, ""sv},  // V4：只有 --manual-steamvr（T3 的編排在 V5）
      {"T5"sv, "V5"sv},  // hold：V4 以 T4 的 --hold-sec 代替
      {"T6"sv, ""sv},
      {"T7"sv, "V6"sv},
      {"CLEANUP"sv, "V5"sv},
    };

    const std::vector<std::string> k_default_only {"T0", "T1", "T1B", "T2", "T6"};

    std::string upper_trim(std::string_view s) {
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
      }
      while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
      }
      std::string out {s};
      std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
      });
      return out;
    }

    bool parse_int(std::string_view s, int &out) {
      if (s.empty()) {
        return false;
      }
      auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
      return ec == std::errc {} && p == s.data() + s.size();
    }

    /// CLI 旗標 → JSON 鍵與型別
    enum class arg_kind_e {
      string,
      integer,
      flag,
    };

    struct cli_arg_t {
      std::string_view flag;  ///< 不含前導 `--`
      std::string_view key;
      arg_kind_e kind;
    };

    constexpr cli_arg_t k_cli_args[] = {
      {"only"sv, "only"sv, arg_kind_e::string},
      {"cycles"sv, "cycles"sv, arg_kind_e::integer},
      {"probe"sv, "probe"sv, arg_kind_e::string},
      {"motion"sv, "motion"sv, arg_kind_e::string},
      {"hold-sec"sv, "hold_sec"sv, arg_kind_e::integer},
      {"reset-seated"sv, "reset_seated"sv, arg_kind_e::flag},
      {"preset"sv, "preset"sv, arg_kind_e::string},
      {"dry-run"sv, "dry_run"sv, arg_kind_e::flag},
      {"manual-steamvr"sv, "manual_steamvr"sv, arg_kind_e::flag},
      {"attach-session"sv, "attach_session"sv, arg_kind_e::flag},
      {"detach"sv, "detach"sv, arg_kind_e::flag},
      {"probe-path"sv, "probe_path"sv, arg_kind_e::string},
    };

    // ── 與 vr_clock（S1-12）的接縫：估計器／pacing 情境由時鐘模組提供（純函式、決定性），這裡只負責呼叫與計分 ──
    void run_t6_clock(reporter_t &r) {
      const auto results = vr::clk::run_selftests();
      if (results.empty()) {
        r.check("T6.clk"sv, false, "cases=0 (vr_clock provided no selftest cases)"sv);
        return;
      }
      for (const auto &sc : results) {
        // name 已帶 `clk.` 前綴（例 clk.offset-jitter-drift50）
        r.check("T6."s + sc.name, sc.pass, sc.detail);
      }
    }

    /// T6 的平台中立部分：vr_clock_* 的基本性質（Windows 另外在 platform::run_t6 驗 QPC 換算）
    void run_t6_neutral(reporter_t &r) {
      const int64_t f = platf::vr_clock_frequency();
      r.check("T6.vr-clock-freq"sv, f > 0, std::format("freq={}", f));

      int64_t prev = platf::vr_clock_ticks();
      int64_t backwards = 0;
      for (int i = 0; i < 10000; ++i) {
        const int64_t now = platf::vr_clock_ticks();
        if (now < prev) {
          ++backwards;
        }
        prev = now;
      }
      r.check("T6.vr-clock-monotonic"sv, backwards == 0, std::format("reads=10000 backwards={}", backwards));

      run_t6_clock(r);
    }
  }  // namespace

  std::optional<std::string> fetch_serverinfo_xml(std::string &err) {
    // T1b（M1b V3）：從本機 HTTP 埠抓 /serverinfo（未配對也會回 XML）。uniqueid 固定，讓兩次請求條件相同。
    CURL *curl = curl_easy_init();  // NOSONAR
    if (!curl) {
      err = "curl_easy_init failed";
      return std::nullopt;
    }
    std::string body;
    const auto url = std::format("http://127.0.0.1:{}/serverinfo?uniqueid=0123456789ABCDEF", (int) config::sunshine.port);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char *ptr, size_t size, size_t nmemb, void *userdata) -> size_t {
      static_cast<std::string *>(userdata)->append(ptr, size * nmemb);
      return size * nmemb;
    });
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    const CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status != 200) {
      err = std::format("curl={} http={}", (int) rc, status);
      return std::nullopt;
    }
    return body;
  }

  // ── reporter ────────────────────────────────────────────────────────

  reporter_t::reporter_t(sink_t sink):
      sink_ {std::move(sink)} {
  }

  void reporter_t::emit(const std::string &line, bool to_log, bool is_failure) {
    if (to_log) {
      // 成員函式 info()／verbose() 會遮住全域的 logger，這裡一律寫 ::
      if (is_failure) {
        BOOST_LOG(::warning) << line;
      } else {
        BOOST_LOG(::info) << line;
      }
    }
    if (sink_) {
      sink_(line);
    }
  }

  void reporter_t::check(std::string_view id, bool pass, std::string_view detail) {
    if (pass) {
      ++pass_;
    } else {
      ++fail_;
    }
    std::string line {k_prefix};
    line.append(id).append(pass ? " result=PASS"sv : " result=FAIL"sv);
    if (!detail.empty()) {
      line.append(" ").append(detail);
    }
    emit(line, true, !pass);
  }

  void reporter_t::info(std::string_view id, std::string_view detail) {
    std::string line {k_prefix};
    line.append(id).append(" result=INFO"sv);
    if (!detail.empty()) {
      line.append(" ").append(detail);
    }
    emit(line, true, false);
  }

  void reporter_t::not_run(std::string_view id, std::string_view until, std::string_view detail) {
    ++not_run_;
    std::string line {k_prefix};
    line.append(id).append(" result=NOT-RUN until="sv).append(until);
    if (!detail.empty()) {
      line.append(" ").append(detail);
    }
    emit(line, true, false);
  }

  void reporter_t::verbose(std::string_view id, std::string_view text) {
    std::string line {k_prefix};
    line.append(id).append(" ").append(text);
    emit(line, false, false);
  }

  void reporter_t::line(std::string_view text) {
    std::string line {k_prefix};
    line.append(text);
    emit(line, true, false);
  }

  // ── vr_probe 輸出過濾（sec-m14）──────────────────────────────────────

  std::optional<std::string> sanitize_probe_line(std::string_view raw) {
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == '\n')) {
      raw.remove_suffix(1);
    }
    if (!raw.starts_with(k_probe_prefix)) {
      return std::nullopt;
    }
    raw.remove_prefix(k_probe_prefix.size());
    if (raw.size() > k_probe_line_max) {
      raw = raw.substr(0, k_probe_line_max);
    }
    std::string out {raw};
    for (auto &c : out) {
      const auto u = static_cast<unsigned char>(c);
      if (u < 0x20 || u >= 0x7F || c == '[') {
        c = '?';
      }
    }
    return out;
  }

  std::optional<std::string> probe_kv_t::get(std::string_view key) const {
    for (const auto &[k, v] : kv) {
      if (k == key) {
        return v;
      }
    }
    return std::nullopt;
  }

  std::optional<int64_t> probe_kv_t::get_int(std::string_view key) const {
    auto v = get(key);
    if (!v || v->empty()) {
      return std::nullopt;
    }
    int64_t out = 0;
    const char *b = v->data();
    const char *e = v->data() + v->size();
    int base = 10;
    if (v->size() > 2 && (*v)[0] == '0' && ((*v)[1] == 'x' || (*v)[1] == 'X')) {
      b += 2;
      base = 16;
    }
    auto [p, ec] = std::from_chars(b, e, out, base);
    if (ec != std::errc {} || p != e) {
      return std::nullopt;
    }
    return out;
  }

  probe_kv_t parse_probe_kv(std::string_view s) {
    probe_kv_t out;
    int bare = 0;
    while (!s.empty()) {
      const auto sp = s.find(' ');
      const std::string_view tok = s.substr(0, sp);
      s = sp == std::string_view::npos ? std::string_view {} : s.substr(sp + 1);
      if (tok.empty()) {
        continue;
      }
      const auto eq = tok.find('=');
      if (eq == std::string_view::npos) {
        if (bare == 0) {
          out.tag = std::string {tok};
        } else if (bare == 1) {
          out.sub = std::string {tok};
        }
        ++bare;
      } else if (eq > 0) {
        out.kv.emplace_back(std::string {tok.substr(0, eq)}, std::string {tok.substr(eq + 1)});
      }
    }
    return out;
  }

  // ── 請求解析 ────────────────────────────────────────────────────────

  std::optional<std::string> cli_args_to_json(int argc, char **argv, std::string &err) {
    nlohmann::json args = nlohmann::json::object();
    for (int i = 0; i < argc; ++i) {
      std::string_view a = argv[i] ? std::string_view {argv[i]} : std::string_view {};
      if (!a.starts_with("--"sv)) {
        err = std::format("unexpected argument '{}' (options start with --)", a);
        return std::nullopt;
      }
      a.remove_prefix(2);
      std::optional<std::string_view> inline_value;
      if (const auto eq = a.find('='); eq != std::string_view::npos) {
        inline_value = a.substr(eq + 1);
        a = a.substr(0, eq);
      }
      const auto *def = std::find_if(std::begin(k_cli_args), std::end(k_cli_args), [&](const cli_arg_t &d) {
        return d.flag == a;
      });
      if (def == std::end(k_cli_args)) {
        err = std::format("unknown option --{}", a);
        return std::nullopt;
      }
      if (def->kind == arg_kind_e::flag) {
        if (inline_value) {
          err = std::format("option --{} takes no value", a);
          return std::nullopt;
        }
        args[std::string {def->key}] = true;
        continue;
      }
      std::string_view value;
      if (inline_value) {
        value = *inline_value;
      } else if (i + 1 < argc && argv[i + 1]) {
        value = argv[++i];
      } else {
        err = std::format("option --{} needs a value", a);
        return std::nullopt;
      }
      if (def->kind == arg_kind_e::integer) {
        int n = 0;
        if (!parse_int(value, n)) {
          err = std::format("option --{} needs an integer, got '{}'", a, value);
          return std::nullopt;
        }
        args[std::string {def->key}] = n;
      } else {
        args[std::string {def->key}] = std::string {value};
      }
    }
    return args.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
  }

  std::optional<request_t> parse_request(std::string_view args_json, std::string &err) {
    nlohmann::json j;
    if (args_json.empty()) {
      j = nlohmann::json::object();
    } else {
      j = nlohmann::json::parse(args_json, nullptr, false);
      if (j.is_discarded()) {
        err = "args is not valid JSON";
        return std::nullopt;
      }
    }
    if (!j.is_object()) {
      err = "args must be a JSON object";
      return std::nullopt;
    }

    request_t req;
    for (const auto &[key, value] : j.items()) {
      const auto *def = std::find_if(std::begin(k_cli_args), std::end(k_cli_args), [&](const cli_arg_t &d) {
        return d.key == key;
      });
      if (def == std::end(k_cli_args)) {
        err = std::format("unknown key '{}'", key);
        return std::nullopt;
      }
      const bool type_ok = (def->kind == arg_kind_e::flag && value.is_boolean()) ||
                           (def->kind == arg_kind_e::integer && value.is_number_integer()) ||
                           (def->kind == arg_kind_e::string && value.is_string());
      if (!type_ok) {
        err = std::format("key '{}' has the wrong type", key);
        return std::nullopt;
      }
    }

    // only：逗號分隔，不分大小寫；去重後依 k_tests 的固定順序
    std::set<std::string> wanted;
    if (auto it = j.find("only"); it != j.end()) {
      std::string_view list = it->get_ref<const std::string &>();
      while (!list.empty()) {
        const auto comma = list.find(',');
        auto name = upper_trim(list.substr(0, comma));
        list = comma == std::string_view::npos ? std::string_view {} : list.substr(comma + 1);
        if (name.empty()) {
          continue;
        }
        const auto *def = std::find_if(std::begin(k_tests), std::end(k_tests), [&](const test_def_t &t) {
          return t.name == name;
        });
        if (def == std::end(k_tests)) {
          err = std::format("unknown sub-test '{}' (known: T0,T0-secfs,T1,T1b,T2,T3,T4,T5,T6,T7,cleanup)", name);
          return std::nullopt;
        }
        if (!def->implemented_in.empty()) {
          err = std::format("sub-test {} is not implemented until {}", name, def->implemented_in);
          return std::nullopt;
        }
        wanted.insert(std::move(name));
      }
    }
    if (wanted.empty()) {
      req.only = k_default_only;
    } else {
      for (const auto &t : k_tests) {
        if (wanted.contains(std::string {t.name})) {
          req.only.emplace_back(t.name);
        }
      }
    }

    // 先以 int64 取出再檢查範圍，超大的值不會被截斷成看似合法的 int
    auto get_int = [&](const char *key, int def) -> int {
      auto it = j.find(key);
      if (it == j.end()) {
        return def;
      }
      const auto v = it->get<int64_t>();
      return v < -1'000'000 || v > 1'000'000 ? -1'000'000 : static_cast<int>(v);
    };
    auto get_bool = [&](const char *key) {
      auto it = j.find(key);
      return it != j.end() && it->get<bool>();
    };
    auto get_str = [&](const char *key, std::string def) {
      auto it = j.find(key);
      return it == j.end() ? def : it->get<std::string>();
    };

    req.cycles = get_int("cycles", 20);
    req.probe = get_str("probe", "none");
    req.motion = get_str("motion", "still");
    req.hold_sec = get_int("hold_sec", 0);
    req.reset_seated = get_bool("reset_seated");
    req.preset = get_str("preset", "none");
    req.dry_run = get_bool("dry_run");
    req.manual_steamvr = get_bool("manual_steamvr");
    req.attach_session = get_bool("attach_session");
    req.detach = get_bool("detach");
    req.probe_path = get_str("probe_path", "");

    // 範圍（§F.5）；超出範圍一律拒絕
    if (req.cycles < 1 || req.cycles > 1000) {
      err = "cycles must be 1..1000";
      return std::nullopt;
    }
    if (req.hold_sec < 0 || req.hold_sec > 7200) {
      err = "hold_sec must be 0..7200";
      return std::nullopt;
    }
    static const std::set<std::string, std::less<>> probes {"none", "whoami", "space", "timing", "scene", "watch"};
    static const std::set<std::string, std::less<>> motions {"still", "yaw30", "sine"};
    if (!probes.contains(req.probe)) {
      err = "probe must be none|whoami|space|timing|scene|watch";
      return std::nullopt;
    }
    if (!motions.contains(req.motion)) {
      err = "motion must be still|yaw30|sine";
      return std::nullopt;
    }
    if (req.probe_path.size() > 1024) {
      err = "probe_path is too long";
      return std::nullopt;
    }

    // 還沒實作的選項：非預設值不會有任何效果，直接拒絕，不讓使用者誤以為生效
    struct later_t {
      bool set;
      const char *what;
      const char *slice;
    };

    const later_t later[] = {
      {req.preset != "none", "--preset (PoC presets)", "V6"},
      {req.dry_run, "--dry-run (DEPLOY/CONFLICT)", "V5"},
      {req.attach_session, "--attach-session (T4 during a pcvr session)", "V5"},
    };
    for (const auto &l : later) {
      if (l.set) {
        err = std::format("option {} is not implemented until {}", l.what, l.slice);
        return std::nullopt;
      }
    }

    // V4：T4 只有 --manual-steamvr；--probe／--motion／--hold-sec／--reset-seated 只對 T4 有意義
    const bool has_t4 = std::find(req.only.begin(), req.only.end(), "T4") != req.only.end();
    if (has_t4 && !req.manual_steamvr) {
      err = "T4 needs --manual-steamvr until V5 (the T3 SteamVR orchestration)";
      return std::nullopt;
    }
    if (req.manual_steamvr && !has_t4) {
      err = "--manual-steamvr only applies to T4";
      return std::nullopt;
    }
    if (!has_t4 && (req.probe != "none" || req.motion != "still" || req.hold_sec != 0 || req.reset_seated)) {
      err = "--probe/--motion/--hold-sec/--reset-seated only apply to T4";
      return std::nullopt;
    }
    if (req.reset_seated && req.probe != "space") {
      err = "--reset-seated needs --probe space";
      return std::nullopt;
    }
    if (has_t4 && std::find(req.only.begin(), req.only.end(), "T2") != req.only.end()) {
      err = "T2 needs SteamVR closed and T4 needs it running: run them separately";
      return std::nullopt;
    }

    return req;
  }

  // ── 執行 ────────────────────────────────────────────────────────────

  bool running() {
    return g_running.load(std::memory_order_relaxed);
  }

  result_t run(const request_t &req, const std::string &run_id, const sink_t &sink, const std::atomic<bool> &abort) {
    reporter_t r {sink};
    result_t res;

    auto refuse = [&](int rc, std::string reason, std::string text) {
      res.rc = rc;
      res.error = std::move(text);
      r.line(std::format("refused run={} reason={} ({})", run_id, reason, res.error));
      r.line(std::format("(final) pass=0 fail=0 notRun=0 rc={}", rc));
      return res;
    };

    if (!platform::supported()) {
      return refuse(rc_unsupported, "unsupported-platform", "vr selftest is not supported on this platform");
    }
    if (config::vr.pcvr == config::vr_t::pcvr_e::disabled) {
      return refuse(rc_unsupported, "vr-pcvr-disabled", "vr_pcvr=disabled");
    }

    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true)) {
      return refuse(rc_busy, "busy", "another selftest is running");
    }
    struct running_reset_t {
      ~running_reset_t() {
        g_running.store(false);
      }
    } running_reset;

    // K15／ops-M4：開始時不能有任何串流 session（桌面或 VR），也不能有別的 client 的 VR 預約
    if (const int n = rtsp_stream::session_count(); n != 0) {
      return refuse(rc_session_active, "session-active", std::format("{} streaming session(s) active", n));
    }
    if (vr::busy_for(k_reservation_owner)) {
      return refuse(rc_session_active, "vr-busy", "a VR session or reservation of another client exists");
    }
    vr::reserve(k_reservation_owner, k_reservation_ttl);

    // keeper：每 100 ms 轉發外部 abort、檢查是否被串流 session 搶先、檢查整體上限（§F.5：3 h）；每秒續期預約
    std::atomic<bool> stop {false};
    std::atomic<int> stop_reason {0};  // 0 無、1 abort、2 被搶先、3 超過整體上限
    std::mutex keeper_mtx;
    std::condition_variable keeper_cv;
    bool keeper_exit = false;
    const auto t_start = std::chrono::steady_clock::now();
    std::thread keeper([&]() {
      platf::set_thread_name("vr_selftest_keeper");
      auto last_renew = std::chrono::steady_clock::now();
      std::unique_lock lk {keeper_mtx};
      while (!keeper_exit) {
        keeper_cv.wait_for(lk, 100ms);
        if (keeper_exit) {
          break;
        }
        if (abort.load() && !stop.load()) {
          stop_reason.store(1);
          stop.store(true);
        }
        if (rtsp_stream::session_count() != 0 && !stop.load()) {
          stop_reason.store(2);
          stop.store(true);
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - t_start >= k_overall_limit && !stop.load()) {
          stop_reason.store(3);
          stop.store(true);
        }
        if (now - last_renew >= 1s) {
          vr::reserve(k_reservation_owner, k_reservation_ttl);
          last_renew = now;
        }
      }
    });

    std::string only_list;
    for (const auto &t : req.only) {
      if (!only_list.empty()) {
        only_list += ',';
      }
      only_list += t;
    }
    r.line(std::format("start run={} only={} vr_pcvr={} probe={} detach={}", run_id, only_list, config::vr.pcvr == config::vr_t::pcvr_e::stub ? "stub"sv : "other"sv, req.probe_path.empty() ? "default"sv : "override"sv, req.detach ? 1 : 0));

    for (const auto &t : req.only) {
      if (stop.load()) {
        break;
      }
      const int pass0 = r.passed();
      const int fail0 = r.failed();
      const int not_run0 = r.not_run_count();
      const auto t0 = std::chrono::steady_clock::now();
      r.line(std::format("{} begin", t));
      if (t == "T0") {
        platform::run_t0(r, req, stop);
      } else if (t == "T1") {
        platform::run_t1(r, req, stop);
      } else if (t == "T1B") {
        platform::run_t1b(r, req, stop);
      } else if (t == "T2") {
        platform::run_t2(r, req, stop);
      } else if (t == "T3") {
        platform::run_t3(r, req, stop);
      } else if (t == "T4") {
        platform::run_t4(r, req, stop);
      } else if (t == "T6") {
        run_t6_neutral(r);
        if (!stop.load()) {
          platform::run_t6(r, req, stop);
        }
      }
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      r.line(std::format("{} end pass={} fail={} notRun={} ms={}", t, r.passed() - pass0, r.failed() - fail0, r.not_run_count() - not_run0, ms));
    }

    {
      std::lock_guard lk {keeper_mtx};
      keeper_exit = true;
    }
    keeper_cv.notify_all();
    keeper.join();
    vr::clear_reservation();

    res.pass = r.passed();
    res.fail = r.failed();
    res.not_run = r.not_run_count();
    if (stop.load()) {
      const int why = stop_reason.load();
      res.rc = why == 2 ? rc_session_active : rc_aborted;
      res.error = why == 2 ? "a streaming session started during the selftest" : why == 3 ? "overall time limit (3 h) reached" : "aborted";
      r.line(std::format("stopped run={} reason={}", run_id, why == 2 ? "preempted-by-session"sv : why == 3 ? "overall-limit"sv : "abort"sv));
    } else if (res.fail > 0) {
      res.rc = rc_failed;
    } else if (res.pass == 0) {
      res.rc = rc_failed;
      res.error = "no checks ran";
    } else {
      res.rc = rc_ok;
    }

    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_start).count();
    r.line(std::format("(final) pass={} fail={} notRun={} rc={} ms={}", res.pass, res.fail, res.not_run, res.rc, total_ms));
    return res;
  }

#ifndef _WIN32
  // ── 非 Windows：沒有 SteamVR driver 與 VR IPC（S1-19），selftest 一律回報不支援 ──
  namespace platform {
    bool supported() {
      return false;
    }

    void run_t0(reporter_t &r, const request_t &, const std::atomic<bool> &) {
      r.check("T0"sv, false, "unsupported-platform"sv);
    }

    void run_t1(reporter_t &r, const request_t &, const std::atomic<bool> &) {
      r.check("T1"sv, false, "unsupported-platform"sv);
    }

    void run_t1b(reporter_t &r, const request_t &, const std::atomic<bool> &) {
      r.check("T1b"sv, false, "unsupported-platform"sv);
    }

    void run_t2(reporter_t &r, const request_t &, const std::atomic<bool> &) {
      r.check("T2"sv, false, "unsupported-platform"sv);
    }

    void run_t3(reporter_t &r, const request_t &, const std::atomic<bool> &) {
      r.check("T3"sv, false, "unsupported-platform"sv);
    }

    void run_t4(reporter_t &r, const request_t &, const std::atomic<bool> &) {
      r.check("T4"sv, false, "unsupported-platform"sv);
    }

    void run_t6(reporter_t &, const request_t &, const std::atomic<bool> &) {
    }
  }  // namespace platform
#endif
}  // namespace vr::selftest
