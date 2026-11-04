/**
 * @file src/tailscale_path.cpp
 * @brief §TS-PATH：串流中若 Tailscale 路徑掉到中繼，依序 ping、restun、rebind 嘗試打回直連。
 *
 * rebind 只重建 UDP socket，DERP（TCP）那條不受影響，不該把正在走的串流打斷。
 * 呼叫端的 session 物件這個模組碰不到；停掉時只動自己的旗標。
 */
#include "tailscale_path.h"

#include "logging.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <boost/process/v1.hpp>
#include <boost/process/v1/args.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/handles.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/search_path.hpp>
#ifdef _WIN32
  #include <boost/process/v1/windows.hpp>
#else
  #include <signal.h>
  #include <sys/types.h>
  #include <sys/wait.h>
#endif

#include <nlohmann/json.hpp>

using namespace std::literals;
namespace bp = boost::process::v1;

namespace tailscale_path {
  namespace {

    constexpr auto k_poll = 5s;
    constexpr auto k_cmd_timeout = 10s;
    constexpr auto k_slice = 100ms;
    constexpr auto k_stage1 = 10s;
    constexpr auto k_stage2 = 25s;
    constexpr auto k_stage3 = 45s;
    constexpr auto k_rebind_gap = 120s;
    constexpr int k_max_rebinds = 5;
    // Windows：TerminateProcess 不會卡，沿用短收屍。POSIX 不走這條。
    constexpr auto k_reap_budget = 300ms;
    constexpr auto k_posix_reap = 1s;
    constexpr auto k_posix_reap_slice = 50ms;
    constexpr std::size_t k_output_cap = 1024 * 1024;
    constexpr std::size_t k_log_clip = 180;

    using steady = std::chrono::steady_clock;

    enum class path_kind {
      unknown,
      direct,
      relay,
      missing,
    };

    /// 一條監看執行緒。done 在執行緒函式最後設起來，start 時才 join 已經結束的，不 detach。
    struct tracked_worker {
      std::thread thread;
      std::shared_ptr<std::atomic<bool>> done;
    };

    struct engine {
      std::mutex mu;
      std::condition_variable cv;
      // 0＝還沒有監看。之後每次 start／真的停掉目前這次，都會往上加。
      std::atomic<std::uint64_t> generation {0};
      std::atomic<bool> shutting_down {false};
      std::string client_ip;
      std::vector<tracked_worker> workers;
    };

    struct owner {
      std::shared_ptr<engine> eng = std::make_shared<engine>();

      ~owner();
    };

    owner &instance() {
      static owner o;
      return o;
    }

    bool alive(const engine &eng, std::uint64_t gen) {
      return !eng.shutting_down.load(std::memory_order_acquire) && eng.generation.load(std::memory_order_acquire) == gen;
    }

    /// 關機時 logging 的 sink 可能跟著靜態物件一起拆。回傳 false 就不要印，呼叫端要立刻離開這次監看。
    bool log_ok(const engine &eng) {
      return !eng.shutting_down.load(std::memory_order_acquire);
    }

    /// 呼叫端要握著 eng.mu。只搬走 done 已設起的執行緒，還在跑的留在清單裡。
    std::vector<std::thread> take_finished_workers(engine &eng) {
      std::vector<std::thread> out;
      for (auto it = eng.workers.begin(); it != eng.workers.end();) {
        if (it->done && it->done->load(std::memory_order_acquire)) {
          if (it->thread.joinable()) {
            out.push_back(std::move(it->thread));
          }
          it = eng.workers.erase(it);
        } else {
          ++it;
        }
      }
      return out;
    }

    void join_threads(std::vector<std::thread> &threads) {
      for (auto &t : threads) {
        if (t.joinable()) {
          t.join();
        }
      }
    }

    /// 傳回 true 表示這次監看還沒被取代或結束。等待期間會放開鎖，session 結束能立刻叫醒。
    bool sleep_session(engine &eng, std::uint64_t gen, std::chrono::milliseconds dur) {
      std::unique_lock lk(eng.mu);
      eng.cv.wait_for(lk, dur, [&]() {
        return eng.shutting_down.load(std::memory_order_acquire) || eng.generation.load(std::memory_order_acquire) != gen;
      });
      return alive(eng, gen);
    }

    owner::~owner() {
      if (!eng) {
        return;
      }
      // 先讓還在跑的 worker 自己停（子行程會被立刻殺掉），再 join。不要 detach。
      {
        std::lock_guard lk(eng->mu);
        eng->shutting_down.store(true, std::memory_order_release);
      }
      eng->cv.notify_all();
      std::vector<tracked_worker> pending;
      {
        std::lock_guard lk(eng->mu);
        pending.swap(eng->workers);
      }
      for (auto &w : pending) {
        if (w.thread.joinable()) {
          w.thread.join();
        }
      }
    }

    bool parse_ipv4(std::string_view s, int &a, int &b, int &c, int &d) {
      auto octet = [&](std::size_t &i, int &out) {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
          return false;
        }
        int v = 0;
        int digits = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
          v = v * 10 + (s[i] - '0');
          if (v > 255 || ++digits > 3) {
            return false;
          }
          ++i;
        }
        out = v;
        return digits > 0;
      };
      std::size_t i = 0;
      if (!octet(i, a) || i >= s.size() || s[i++] != '.') {
        return false;
      }
      if (!octet(i, b) || i >= s.size() || s[i++] != '.') {
        return false;
      }
      if (!octet(i, c) || i >= s.size() || s[i++] != '.') {
        return false;
      }
      if (!octet(i, d) || i != s.size()) {
        return false;
      }
      return true;
    }

    /// 100.64.0.0/10。IPv4-mapped IPv6（::ffff:100.x.x.x，可帶括號或 zone）也認。其他位址回 nullopt。
    std::optional<std::string> tailscale_cgnat_ipv4(std::string_view in) {
      while (!in.empty() && std::isspace(static_cast<unsigned char>(in.front()))) {
        in.remove_prefix(1);
      }
      while (!in.empty() && std::isspace(static_cast<unsigned char>(in.back()))) {
        in.remove_suffix(1);
      }
      if (in.size() >= 2 && in.front() == '[' && in.back() == ']') {
        in.remove_prefix(1);
        in.remove_suffix(1);
      }
      if (const auto zone = in.find('%'); zone != std::string_view::npos) {
        in = in.substr(0, zone);
      }

      std::string lower;
      lower.reserve(in.size());
      for (unsigned char ch : in) {
        lower.push_back(static_cast<char>(std::tolower(ch)));
      }
      std::string_view addr = in;
      if (const auto pos = lower.rfind("ffff:"); pos != std::string::npos) {
        const auto rest = in.substr(pos + 5);
        int a = 0;
        int b = 0;
        int c = 0;
        int d = 0;
        if (parse_ipv4(rest, a, b, c, d)) {
          addr = rest;
        }
      }

      int a = 0;
      int b = 0;
      int c = 0;
      int d = 0;
      if (!parse_ipv4(addr, a, b, c, d)) {
        return std::nullopt;
      }
      // /10：第二個 octet 64..127。
      if (a != 100 || b < 64 || b > 127) {
        return std::nullopt;
      }
      return std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
    }

    bp::filesystem::path to_bp(const std::filesystem::path &p) {
#ifdef _WIN32
      return bp::filesystem::path(p.wstring());
#else
      return bp::filesystem::path(p.string());
#endif
    }

    bp::filesystem::path find_cli() {
#ifdef _WIN32
      // 服務以 SYSTEM 跑時，PATH 常常沒有 Tailscale，先看安裝目錄。
      if (const char *pf = std::getenv("ProgramFiles"); pf != nullptr && *pf != '\0') {
        const std::filesystem::path candidate = std::filesystem::path(pf) / "Tailscale" / "tailscale.exe";
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
          return to_bp(candidate);
        }
      }
      if (auto found = bp::search_path("tailscale"); !found.empty()) {
        return found;
      }
      return {};
#else
      if (auto found = bp::search_path("tailscale"); !found.empty()) {
        return found;
      }
      for (const char *candidate : {"/usr/bin/tailscale", "/usr/local/bin/tailscale"}) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec)) {
          return bp::filesystem::path(candidate);
        }
      }
      return {};
#endif
    }

    struct cmd_result {
      bool launched = false;
      bool timed_out = false;
      bool interrupted = false;
      int exit_code = -1;
      std::string stdout_text;
      std::string stderr_text;
      std::string error;
    };

    struct file_guard {
      std::filesystem::path path;

      ~file_guard() {
        if (!path.empty()) {
          std::error_code ec;
          std::filesystem::remove(path, ec);
        }
      }
    };

    std::string slurp(const std::filesystem::path &path) {
      std::ifstream in(path, std::ios::in | std::ios::binary);
      if (!in) {
        return {};
      }
      std::string s;
      char buf[4096];
      while (in && s.size() < k_output_cap) {
        in.read(buf, sizeof(buf));
        const auto n = static_cast<std::size_t>(in.gcount());
        if (n == 0) {
          break;
        }
        const auto room = k_output_cap - s.size();
        s.append(buf, std::min(room, n));
      }
      return s;
    }

    void kill_child(engine &eng, bp::child &child) {
#ifdef _WIN32
      // TerminateProcess 不會卡在 wait。這裡只是等它真的結束，等不到就放掉把手。
      (void) eng;
      std::error_code ec;
      child.terminate(ec);
      const auto deadline = steady::now() + k_reap_budget;
      while (steady::now() < deadline) {
        if (!child.running(ec)) {
          return;
        }
        std::this_thread::sleep_for(20ms);
      }
      child.detach();
#else
      // 不要用 bp::child::terminate()：POSIX 實作是 kill 之後無逾時的 waitpid。
      const auto pid = static_cast<pid_t>(child.id());
      if (pid > 0) {
        ::kill(pid, SIGKILL);
        int st = 0;
        const auto deadline = steady::now() + k_posix_reap;
        bool reaped = false;
        while (steady::now() < deadline) {
          const pid_t got = ::waitpid(pid, &st, WNOHANG);
          if (got == pid || (got < 0 && errno == ECHILD)) {
            reaped = true;
            break;
          }
          std::this_thread::sleep_for(k_posix_reap_slice);
        }
        if (!reaped && log_ok(eng)) {
          BOOST_LOG(warning) << "[VIPLE-TS-PATH] pid "sv << pid << " still alive 1s after SIGKILL; detached"sv;
        }
      }
      // 成功收屍也要 detach，避免解構子再對同一個 pid 做一次 waitpid（pid 可能已被重用）。
      child.detach();
#endif
    }

    cmd_result run_cmd(engine &eng, std::uint64_t gen, const bp::filesystem::path &cli, const std::vector<std::string> &args) {
      cmd_result r;
      if (!alive(eng, gen)) {
        r.interrupted = true;
        return r;
      }

      static std::atomic<std::uint64_t> seq {0};
      std::filesystem::path out_path;
      std::filesystem::path err_path;
      try {
        const auto dir = std::filesystem::temp_directory_path();
        const auto id = seq.fetch_add(1, std::memory_order_relaxed);
        out_path = dir / ("viple-ts-path-" + std::to_string(id) + ".out");
        err_path = dir / ("viple-ts-path-" + std::to_string(id) + ".err");
        std::ofstream(out_path, std::ios::trunc | std::ios::binary).close();
        std::ofstream(err_path, std::ios::trunc | std::ios::binary).close();
      } catch (const std::exception &e) {
        r.error = e.what();
        return r;
      }
      file_guard out_guard {out_path};
      file_guard err_guard {err_path};

      // child 放在 try 外面：丟例外時先用 kill_child 收掉，避免解構子再呼叫無逾時的 terminate()。
      std::optional<bp::child> child;
      try {
#ifdef _WIN32
        // create_no_window：不要為 console 子系統的 tailscale.exe 配主控台。
        // hide：若仍有視窗，STARTF_USESHOWWINDOW + SW_HIDE。
        // 不用 bp::limit_handles：Windows 實作會 NtQuerySystemInformation 掃整個系統的 handle，
        // 每 5 秒一次太重，失敗還會丟例外。隱藏視窗靠下面兩個 flag，跟 misc.cpp 的 CREATE_NO_WINDOW 同一條路。
        child.emplace(
          cli,
          bp::args(args),
          bp::std_out > to_bp(out_path),
          bp::std_err > to_bp(err_path),
          bp::std_in < bp::null,
          bp::windows::create_no_window,
          bp::windows::hide
        );
#else
        child.emplace(
          cli,
          bp::args(args),
          bp::std_out > to_bp(out_path),
          bp::std_err > to_bp(err_path),
          bp::std_in < bp::null,
          bp::limit_handles
        );
#endif
        r.launched = true;
        const auto deadline = steady::now() + k_cmd_timeout;
        bool saw_exit = false;
        while (steady::now() < deadline) {
          if (!alive(eng, gen)) {
            kill_child(eng, *child);
            r.interrupted = true;
            return r;
          }
          std::error_code ec;
          if (!child->running(ec)) {
            if (!ec) {
              r.exit_code = child->exit_code();
              saw_exit = true;
            } else {
              r.error = ec.message();
            }
            break;
          }
          const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - steady::now());
          if (left.count() <= 0) {
            break;
          }
          if (!sleep_session(eng, gen, std::min(std::chrono::milliseconds {k_slice}, left))) {
            kill_child(eng, *child);
            r.interrupted = true;
            return r;
          }
        }
        if (!r.interrupted && !saw_exit) {
          std::error_code ec;
          if (child->running(ec)) {
            kill_child(eng, *child);
            r.timed_out = true;
          } else if (!ec) {
            r.exit_code = child->exit_code();
          } else if (r.error.empty()) {
            r.error = ec.message();
          }
        }
      } catch (const std::exception &e) {
        if (child) {
          kill_child(eng, *child);
        }
        r.error = e.what();
        r.launched = false;
        return r;
      }

      r.stdout_text = slurp(out_path);
      r.stderr_text = slurp(err_path);
      return r;
    }

    std::string one_line(std::string_view text, std::size_t cap = k_log_clip) {
      std::string s;
      s.reserve(std::min(text.size(), cap));
      bool space = false;
      for (unsigned char ch : text) {
        if (s.size() >= cap) {
          break;
        }
        if (ch < 0x20 || ch == 0x7f) {
          if (!s.empty() && !space) {
            s.push_back(' ');
            space = true;
          }
          continue;
        }
        s.push_back(static_cast<char>(ch));
        space = false;
      }
      while (!s.empty() && s.back() == ' ') {
        s.pop_back();
      }
      return s;
    }

    bool contains_ci(std::string_view hay, std::string_view needle) {
      if (needle.empty() || hay.size() < needle.size()) {
        return false;
      }
      auto lower = [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      };
      for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < needle.size(); ++j) {
          if (lower(static_cast<unsigned char>(hay[i + j])) != lower(static_cast<unsigned char>(needle[j]))) {
            match = false;
            break;
          }
        }
        if (match) {
          return true;
        }
      }
      return false;
    }

    bool has_privilege_text(std::string_view text) {
      // 錯誤訊息通常很短；掃整份 status JSON 會被主機名稱裡的 "permission" 誤傷。
      constexpr std::size_t k_scan = 16384;
      if (text.size() > k_scan) {
        text = text.substr(0, k_scan);
      }
      return contains_ci(text, "access denied") || contains_ci(text, "permission") || contains_ci(text, "--operator");
    }

    std::string describe_cmd(const cmd_result &r) {
      if (!r.launched && !r.error.empty()) {
        return "launch failed: " + one_line(r.error);
      }
      if (r.timed_out) {
        return "timed out after 10s, killed";
      }
      const auto blob = one_line(r.stderr_text.empty() ? std::string_view {r.stdout_text} : std::string_view {r.stderr_text});
      if (blob.empty() && r.exit_code == 0) {
        return "ok";
      }
      if (blob.empty()) {
        return "exit=" + std::to_string(r.exit_code);
      }
      return blob;
    }

    std::string describe_ping(const cmd_result &r) {
      if (!r.launched && !r.error.empty()) {
        return "launch failed: " + one_line(r.error);
      }
      if (r.timed_out) {
        return "timed out after 10s, killed";
      }
      const std::string blob = r.stdout_text + "\n" + r.stderr_text;
      if (contains_ci(blob, "via DERP")) {
        return "via DERP";
      }
      if (contains_ci(blob, "direct connection not established")) {
        return "no direct";
      }
      if (blob.find("via ") != std::string::npos) {
        return "via endpoint";
      }
      return describe_cmd(r);
    }

    struct session {
      std::shared_ptr<engine> eng;
      std::uint64_t gen = 0;
      std::string ip;
      bp::filesystem::path cli;
      path_kind path = path_kind::unknown;
      std::optional<steady::time_point> relay_since;
      int stage = 0;
      int rebinds = 0;
      std::optional<steady::time_point> last_rebind;
      bool privilege_blocked = false;
      bool status_fault = false;
      bool cap_logged = false;
    };

    bool session_alive(const session &s) {
      return s.eng && alive(*s.eng, s.gen);
    }

    std::int64_t relay_seconds(const session &s) {
      if (!s.relay_since) {
        return 0;
      }
      return std::chrono::duration_cast<std::chrono::seconds>(steady::now() - *s.relay_since).count();
    }

    /// exit_counts 只給 restun／rebind。ping 在還沒打通時常常非 0，不能拿來停掉後面的階段。
    /// 回傳 false：關機中，呼叫端要立刻離開這次監看。
    bool note_privilege(session &s, const cmd_result &r, bool exit_counts, bool scan_stdout) {
      if (s.privilege_blocked || r.interrupted) {
        return true;
      }
      const bool text = has_privilege_text(r.stderr_text) || has_privilege_text(r.error) || (scan_stdout && has_privilege_text(r.stdout_text));
      const bool code = exit_counts && r.launched && !r.timed_out && r.exit_code != 0;
      if (!text && !code) {
        return true;
      }
      s.privilege_blocked = true;
      if (!log_ok(*s.eng)) {
        return false;
      }
      BOOST_LOG(warning) << "[VIPLE-TS-PATH] no privilege for restun/rebind; ping only for the rest of this session"sv;
      return true;
    }

    bool run_logged(session &s, const char *name, const std::vector<std::string> &args, bool exit_counts, bool ping) {
      auto r = run_cmd(*s.eng, s.gen, s.cli, args);
      if (r.interrupted || !session_alive(s)) {
        return false;
      }
      if (!log_ok(*s.eng)) {
        return false;
      }
      if (ping) {
        BOOST_LOG(info) << "[VIPLE-TS-PATH] "sv << name << " "sv << s.ip << " -> "sv << describe_ping(r) << " exit="sv << r.exit_code;
      } else {
        BOOST_LOG(info) << "[VIPLE-TS-PATH] "sv << name << " -> "sv << describe_cmd(r) << " exit="sv << r.exit_code;
      }
      if (!note_privilege(s, r, exit_counts, true)) {
        return false;
      }
      return session_alive(s);
    }

    bool do_ping(session &s) {
      return run_logged(s, "ping", {"ping", "--c", "3", "--timeout", "3s", s.ip}, false, true);
    }

    bool do_restun(session &s) {
      return run_logged(s, "restun", {"debug", "restun"}, true, false);
    }

    bool do_rebind(session &s) {
      auto r = run_cmd(*s.eng, s.gen, s.cli, {"debug", "rebind"});
      if (r.interrupted || !session_alive(s)) {
        return false;
      }
      ++s.rebinds;
      s.last_rebind = steady::now();
      if (!log_ok(*s.eng)) {
        return false;
      }
      BOOST_LOG(info) << "[VIPLE-TS-PATH] rebind "sv << s.rebinds << "/"sv << k_max_rebinds << " -> "sv << describe_cmd(r) << " exit="sv << r.exit_code;
      if (!note_privilege(s, r, true, true)) {
        return false;
      }
      return session_alive(s);
    }

    struct peer_obs {
      bool found = false;
      bool direct = false;
      std::string cur;
      std::string relay;
    };

    std::string json_string(const nlohmann::json &obj, const char *key) {
      const auto it = obj.find(key);
      if (it == obj.end() || it->is_null() || !it->is_string()) {
        return {};
      }
      return it->get<std::string>();
    }

    std::optional<peer_obs> find_peer(const nlohmann::json &root, const std::string &ip) {
      if (!root.is_object()) {
        return std::nullopt;
      }
      peer_obs obs;
      const auto peers = root.find("Peer");
      if (peers == root.end() || peers->is_null()) {
        return obs;
      }
      if (!peers->is_object()) {
        return std::nullopt;
      }
      for (auto it = peers->begin(); it != peers->end(); ++it) {
        if (!it->is_object()) {
          continue;
        }
        const auto ips = it->find("TailscaleIPs");
        if (ips == it->end() || !ips->is_array()) {
          continue;
        }
        bool match = false;
        for (const auto &one : *ips) {
          if (!one.is_string()) {
            continue;
          }
          const auto text = one.get<std::string>();
          if (text == ip) {
            match = true;
            break;
          }
          if (const auto norm = tailscale_cgnat_ipv4(text); norm && *norm == ip) {
            match = true;
            break;
          }
        }
        if (!match) {
          continue;
        }
        obs.found = true;
        obs.cur = json_string(*it, "CurAddr");
        obs.relay = json_string(*it, "Relay");
        obs.direct = !obs.cur.empty();
        return obs;
      }
      return obs;
    }

    std::optional<nlohmann::json> parse_status(const std::string &text) {
      if (text.empty()) {
        return std::nullopt;
      }
      auto parsed = nlohmann::json::parse(text, nullptr, false);
      if (!parsed.is_discarded() && parsed.is_object()) {
        return parsed;
      }
      const auto brace = text.find('{');
      if (brace == std::string::npos) {
        return std::nullopt;
      }
      parsed = nlohmann::json::parse(text.substr(brace), nullptr, false);
      if (parsed.is_discarded() || !parsed.is_object()) {
        return std::nullopt;
      }
      return parsed;
    }

    /// 回傳 false：關機中，呼叫端要立刻離開這次監看。
    bool note_path(session &s, const peer_obs &obs) {
      if (!obs.found) {
        if (s.path != path_kind::missing) {
          if (!log_ok(*s.eng)) {
            return false;
          }
          BOOST_LOG(info) << "[VIPLE-TS-PATH] peer "sv << s.ip << " not in status"sv;
          s.path = path_kind::missing;
        }
        return true;
      }
      if (obs.direct) {
        if (s.path == path_kind::relay && s.relay_since) {
          if (!log_ok(*s.eng)) {
            return false;
          }
          BOOST_LOG(info) << "[VIPLE-TS-PATH] peer "sv << s.ip << " relay -> direct after "sv << relay_seconds(s) << "s cur="sv << obs.cur;
        } else if (s.path != path_kind::direct) {
          if (!log_ok(*s.eng)) {
            return false;
          }
          BOOST_LOG(info) << "[VIPLE-TS-PATH] peer "sv << s.ip << " direct cur="sv << obs.cur;
        }
        s.path = path_kind::direct;
        // 回到直連就重走升級階梯；本 session 的 rebind 次數不重置。
        s.stage = 0;
        s.relay_since.reset();
        return true;
      }
      const char *region = obs.relay.empty() ? "?" : obs.relay.c_str();
      if (s.path != path_kind::relay) {
        if (!log_ok(*s.eng)) {
          return false;
        }
        if (s.path == path_kind::direct) {
          BOOST_LOG(info) << "[VIPLE-TS-PATH] peer "sv << s.ip << " direct -> relay region="sv << region;
        } else {
          BOOST_LOG(info) << "[VIPLE-TS-PATH] peer "sv << s.ip << " on relay region="sv << region;
        }
        s.path = path_kind::relay;
        s.relay_since = steady::now();
        s.stage = 0;
      }
      return true;
    }

    /// 一次狀態輪詢只做一個階段，做完先再讀 status，避免 ping 還沒生效就 rebind。
    bool escalate(session &s) {
      if (s.path != path_kind::relay || !s.relay_since) {
        return true;
      }
      const auto elapsed = steady::now() - *s.relay_since;
      if (s.stage <= 0) {
        if (elapsed < k_stage1) {
          return true;
        }
        if (!do_ping(s)) {
          return false;
        }
        s.stage = 1;
        return session_alive(s);
      }
      if (s.stage == 1) {
        if (elapsed < k_stage2) {
          return true;
        }
        if (!s.privilege_blocked && !do_restun(s)) {
          return false;
        }
        if (!session_alive(s) || !do_ping(s)) {
          return false;
        }
        s.stage = 2;
        return session_alive(s);
      }
      if (s.stage == 2) {
        if (elapsed < k_stage3) {
          return true;
        }
        if (!s.privilege_blocked && s.rebinds < k_max_rebinds) {
          if (!do_rebind(s)) {
            return false;
          }
        } else if (s.rebinds >= k_max_rebinds && !s.cap_logged) {
          s.cap_logged = true;
          if (!log_ok(*s.eng)) {
            return false;
          }
          BOOST_LOG(info) << "[VIPLE-TS-PATH] rebind cap "sv << k_max_rebinds << " reached; ping only"sv;
        }
        if (!session_alive(s) || !do_ping(s)) {
          return false;
        }
        s.stage = 3;
        return session_alive(s);
      }
      // 階段 3 之後：每 120 秒最多再 rebind 一次，本 session 總共不超過 5 次。這裡不再加 ping。
      if (s.privilege_blocked || s.rebinds >= k_max_rebinds) {
        if (s.rebinds >= k_max_rebinds && !s.cap_logged) {
          s.cap_logged = true;
          if (!log_ok(*s.eng)) {
            return false;
          }
          BOOST_LOG(info) << "[VIPLE-TS-PATH] rebind cap "sv << k_max_rebinds << " reached; not rebinding again this session"sv;
        }
        return true;
      }
      if (s.last_rebind && steady::now() - *s.last_rebind < k_rebind_gap) {
        return true;
      }
      return do_rebind(s);
    }

    bool poll_status(session &s) {
      auto r = run_cmd(*s.eng, s.gen, s.cli, {"status", "--json"});
      if (r.interrupted || !session_alive(s)) {
        return false;
      }
      const bool stdout_is_text = r.stdout_text.find('{') == std::string::npos;
      if (!note_privilege(s, r, false, stdout_is_text)) {
        return false;
      }

      auto parsed = (r.launched && !r.timed_out && r.exit_code == 0) ? parse_status(r.stdout_text) : std::optional<nlohmann::json> {};
      if (!parsed) {
        if (!s.status_fault) {
          s.status_fault = true;
          const auto detail = one_line(r.stderr_text.empty() ? std::string_view {r.stdout_text} : std::string_view {r.stderr_text});
          if (!log_ok(*s.eng)) {
            return false;
          }
          BOOST_LOG(warning) << "[VIPLE-TS-PATH] status failed exit="sv << r.exit_code
                             << (r.timed_out ? " timed out"sv : ""sv)
                             << (r.error.empty() ? ""sv : " err="sv)
                             << r.error
                             << (detail.empty() ? ""sv : " detail="sv)
                             << detail;
        }
        return session_alive(s);
      }
      if (s.status_fault) {
        s.status_fault = false;
        if (!log_ok(*s.eng)) {
          return false;
        }
        BOOST_LOG(info) << "[VIPLE-TS-PATH] status ok"sv;
      }
      const auto obs = find_peer(*parsed, s.ip);
      if (!obs) {
        if (!s.status_fault) {
          s.status_fault = true;
          if (!log_ok(*s.eng)) {
            return false;
          }
          BOOST_LOG(warning) << "[VIPLE-TS-PATH] status JSON has no Peer object"sv;
        }
        return session_alive(s);
      }
      if (!note_path(s, *obs)) {
        return false;
      }
      if (!session_alive(s)) {
        return false;
      }
      return escalate(s);
    }

    void watch_session(std::shared_ptr<engine> eng, std::uint64_t gen, std::string ip) {
      if (!alive(*eng, gen)) {
        return;
      }
      const auto cli = find_cli();
      if (cli.empty()) {
        // 找不到就記一次然後結束這次監看，不要每 5 秒再找、再印。
        if (alive(*eng, gen)) {
          if (!log_ok(*eng)) {
            return;
          }
          BOOST_LOG(warning) << "[VIPLE-TS-PATH] tailscale CLI not found; stop watching "sv << ip;
        }
        return;
      }
      if (!alive(*eng, gen) || !log_ok(*eng)) {
        return;
      }
      BOOST_LOG(info) << "[VIPLE-TS-PATH] watching "sv << ip << " cli="sv << cli.string();

      session s;
      s.eng = std::move(eng);
      s.gen = gen;
      s.ip = std::move(ip);
      s.cli = cli;

      while (session_alive(s)) {
        const auto cycle = steady::now();
        if (!poll_status(s) || !session_alive(s)) {
          return;
        }
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - cycle);
        const auto period = std::chrono::duration_cast<std::chrono::milliseconds>(k_poll);
        if (spent < period && !sleep_session(*s.eng, s.gen, period - spent)) {
          return;
        }
      }
    }

  }  // namespace

  std::uint64_t on_session_start(const std::string &client_ip) {
    const auto ip = tailscale_cgnat_ipv4(client_ip);
    if (!ip) {
      return 0;
    }
    auto &slot = instance();
    auto eng = slot.eng;
    if (!eng || eng->shutting_down.load(std::memory_order_acquire)) {
      return 0;
    }

    std::vector<std::thread> finished;
    std::uint64_t gen = 0;
    bool started = false;
    std::string start_error;
    {
      std::lock_guard lk(eng->mu);
      if (!eng->shutting_down.load(std::memory_order_acquire)) {
        // 順手收掉已經結束的舊監看。還在跑的留在清單裡，看到代號變了會自己停。
        finished = take_finished_workers(*eng);
        gen = eng->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (gen == 0) {
          // 0 留給「沒有監看」。繞回時再加一次。
          gen = eng->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        }
        eng->client_ip = *ip;
        auto done = std::make_shared<std::atomic<bool>>(false);
        try {
          std::thread worker([eng, gen, ip = *ip, done]() {
            try {
              try {
                watch_session(eng, gen, ip);
              } catch (const std::exception &e) {
                if (log_ok(*eng)) {
                  BOOST_LOG(error) << "[VIPLE-TS-PATH] watcher failed: "sv << e.what();
                }
              } catch (...) {
                if (log_ok(*eng)) {
                  BOOST_LOG(error) << "[VIPLE-TS-PATH] watcher failed"sv;
                }
              }
            } catch (...) {
            }
            done->store(true, std::memory_order_release);
          });
          eng->workers.push_back(tracked_worker {std::move(worker), std::move(done)});
          started = true;
        } catch (const std::exception &e) {
          // 執行緒沒生出來。鎖還在手上，代號若沒被人再加過就退回，舊監看繼續。
          if (eng->generation.load(std::memory_order_relaxed) == gen) {
            eng->generation.store(gen - 1, std::memory_order_release);
          }
          start_error = e.what();
        }
      }
    }
    // 先叫醒還在睡的舊監看，再 join 已經結束的。join 不擋這次叫醒。
    if (started) {
      eng->cv.notify_all();
    }
    join_threads(finished);
    if (!start_error.empty() && log_ok(*eng)) {
      BOOST_LOG(warning) << "[VIPLE-TS-PATH] failed to start watcher: "sv << start_error;
    }
    return started ? gen : 0;
  }

  void on_session_end(std::uint64_t gen) {
    if (gen == 0) {
      return;
    }
    auto &slot = instance();
    auto eng = slot.eng;
    if (!eng) {
      return;
    }
    {
      std::lock_guard lk(eng->mu);
      if (eng->shutting_down.load(std::memory_order_acquire)) {
        return;
      }
      // 只有仍是目前這一次才停。新的 session 已經換成別的代號就不要動。
      if (eng->generation.load(std::memory_order_acquire) != gen) {
        return;
      }
      eng->generation.fetch_add(1, std::memory_order_acq_rel);
      eng->client_ip.clear();
    }
    eng->cv.notify_all();
  }

}  // namespace tailscale_path
