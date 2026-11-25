/**
 * @file src/platform/windows/console_session.cpp
 * @brief §CONSOLE-LOGIN：查詢主控台工作階段，必要時把中斷的工作階段接回並鎖定。
 */
#include "src/logging.h"

#include "console_session.h"
#include "utf_utils.h"

#include <Windows.h>
#include <WtsApi32.h>

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

using namespace std::literals;

namespace platf::console {
  namespace {
    struct handle_t {
      HANDLE h = nullptr;

      handle_t() = default;
      explicit handle_t(HANDLE v):
          h(v) {
      }

      handle_t(const handle_t &) = delete;
      handle_t &operator=(const handle_t &) = delete;

      // 有自訂複製建構就不會自動產生移動。spawn 要把行程控制碼交出去，不能複製。
      handle_t(handle_t &&other) noexcept:
          h(other.h) {
        other.h = nullptr;
      }

      handle_t &operator=(handle_t &&other) noexcept {
        if (this != &other) {
          reset();
          h = other.h;
          other.h = nullptr;
        }
        return *this;
      }

      ~handle_t() {
        reset();
      }

      void reset() {
        if (h && h != INVALID_HANDLE_VALUE) {
          CloseHandle(h);
        }
        h = nullptr;
      }

      explicit operator bool() const {
        return h && h != INVALID_HANDLE_VALUE;
      }
    };

    std::wstring system_root() {
      wchar_t buf[MAX_PATH] {};
      const DWORD n = GetEnvironmentVariableW(L"SystemRoot", buf, MAX_PATH);
      if (n == 0 || n >= MAX_PATH) {
        return L"C:\\Windows";
      }
      return buf;
    }

    std::string trim_copy(std::string s) {
      const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
      };
      while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
      }
      while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
      }
      return s;
    }

    std::string session_user(DWORD id) {
      LPWSTR raw = nullptr;
      DWORD bytes = 0;
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, WTSUserName, &raw, &bytes) || !raw) {
        return {};
      }
      const std::wstring wide {raw};
      WTSFreeMemory(raw);
      return trim_copy(utf_utils::to_utf8(wide));
    }

    // 與 vr_platform_win.cpp 的 §VR-HOST-LOCKED 用同一個判斷，兩邊才不會一個說鎖定、一個說沒鎖。
    bool session_locked(DWORD id) {
      PWTSINFOEXW info = nullptr;
      DWORD bytes = 0;
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, WTSSessionInfoEx, reinterpret_cast<LPWSTR *>(&info), &bytes) || !info) {
        return false;
      }
      const bool locked = bytes >= sizeof(WTSINFOEXW) && info->Level == 1 && info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_LOCK;
      WTSFreeMemory(info);
      return locked;
    }

    LONGLONG session_logon(DWORD id) {
      PWTSINFOEXW info = nullptr;
      DWORD bytes = 0;
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, WTSSessionInfoEx, reinterpret_cast<LPWSTR *>(&info), &bytes) || !info) {
        return 0;
      }
      LONGLONG t = 0;
      if (bytes >= sizeof(WTSINFOEXW) && info->Level == 1) {
        t = info->Data.WTSInfoExLevel1.LogonTime.QuadPart;
      }
      WTSFreeMemory(info);
      return t;
    }

    template <class Pred>
    bool wait_pred(std::chrono::milliseconds budget, Pred pred) {
      const auto deadline = std::chrono::steady_clock::now() + budget;
      for (;;) {
        if (pred()) {
          return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
          return false;
        }
        Sleep(100);
      }
    }

    void set_why(std::string *why, const std::string &text) {
      if (why) {
        *why = text;
      }
    }

    bool disconnect_session(DWORD id, std::string *why) {
      // 鎖定沒做成時把工作階段送回去。留著未鎖定的桌面，人不用密碼就看得到。
      if (WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, id, TRUE)) {
        BOOST_LOG(warning) << "[VIPLE-CONSOLE] lock failed; disconnected session=" << id;
        set_why(why, "lock failed; session disconnected again");
        return true;
      }
      const DWORD gle = GetLastError();
      BOOST_LOG(error) << "[VIPLE-CONSOLE] lock failed and WTSDisconnectSession failed session=" << id << " gle=" << gle;
      set_why(why, "lock failed and disconnect failed gle=" + std::to_string(gle));
      return false;
    }

    struct spawned_t {
      handle_t process;
      handle_t thread;
    };

    bool spawn(const std::wstring &exe, std::wstring cmdline, HANDLE token, wchar_t *desktop, spawned_t &out, std::string *why, const char *what) {
      std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
      buf.push_back(L'\0');
      STARTUPINFOW si {};
      si.cb = sizeof(si);
      si.dwFlags = STARTF_USESHOWWINDOW;
      si.wShowWindow = SW_HIDE;
      si.lpDesktop = desktop;
      PROCESS_INFORMATION pi {};
      const BOOL ok = token
                        ? CreateProcessAsUserW(token, exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)
                        : CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
      if (!ok) {
        const DWORD gle = GetLastError();
        BOOST_LOG(warning) << "[VIPLE-CONSOLE] " << what << " CreateProcess failed gle=" << gle;
        set_why(why, std::string(what) + " CreateProcess failed gle=" + std::to_string(gle));
        return false;
      }
      out.thread = handle_t {pi.hThread};
      out.process = handle_t {pi.hProcess};
      return true;
    }

    // 等行程結束。逾時就砍掉。回傳行程是否在時限內結束。
    bool wait_process(HANDLE process, DWORD wait_ms, DWORD *exit_code) {
      const DWORD wait = WaitForSingleObject(process, wait_ms);
      if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 2000);
      }
      if (exit_code) {
        DWORD code = 1;
        if (!GetExitCodeProcess(process, &code)) {
          code = 1;
        }
        *exit_code = code;
      }
      return wait != WAIT_TIMEOUT;
    }
  }  // namespace

  state_t query() {
    state_t st;
    st.console_id = WTSGetActiveConsoleSessionId();
    if (st.console_id != k_no_session) {
      st.user = session_user(st.console_id);
      st.user_on_console = !st.user.empty();
      // 登入畫面沒有使用者，SessionFlags 不算「使用者把桌面鎖起來」。
      st.locked = st.user_on_console && session_locked(st.console_id);
    }

    PWTS_SESSION_INFOW sessions = nullptr;
    DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count) || !sessions) {
      BOOST_LOG(warning) << "[VIPLE-CONSOLE] WTSEnumerateSessionsW failed gle=" << GetLastError();
      return st;
    }

    DWORD best = k_no_session;
    LONGLONG best_logon = -1;
    std::string best_user;
    for (DWORD i = 0; i < count; ++i) {
      const DWORD id = sessions[i].SessionId;
      // session 0 是服務工作階段，不是可以接回主控台的使用者桌面。
      if (sessions[i].State != WTSDisconnected || id == 0 || id == st.console_id) {
        continue;
      }
      auto name = session_user(id);
      if (name.empty()) {
        continue;
      }
      const LONGLONG logon = session_logon(id);
      if (best == k_no_session || logon > best_logon || (logon == best_logon && id > best)) {
        best = id;
        best_logon = logon;
        best_user = std::move(name);
      }
    }
    WTSFreeMemory(sessions);
    st.disconnected_id = best;
    if (!st.user_on_console) {
      st.user = std::move(best_user);
    }
    return st;
  }

  bool reclaim_disconnected(bool lock_after, std::string *why) {
    // /launch 與 /resume 可能同時進來。兩次 tscon 疊在一起會把工作階段又送走。
    static std::mutex reclaim_mu;
    std::lock_guard<std::mutex> guard(reclaim_mu);

    const auto st = query();
    if (st.user_on_console) {
      set_why(why, "console already has a user");
      return false;
    }
    if (st.disconnected_id == k_no_session) {
      set_why(why, "no disconnected session");
      return false;
    }

    const DWORD id = st.disconnected_id;
    const auto root = system_root();
    const std::wstring tscon = root + L"\\System32\\tscon.exe";
    std::wstring tscon_cmd = L"\"" + tscon + L"\" " + std::to_wstring(id) + L" /dest:console";
    BOOST_LOG(info) << "[VIPLE-CONSOLE] tscon session=" << id << " user=" << st.user << " lock_after=" << (lock_after ? 1 : 0);

    spawned_t tscon_proc;
    if (!spawn(tscon, std::move(tscon_cmd), nullptr, nullptr, tscon_proc, why, "tscon")) {
      return false;
    }
    DWORD tscon_exit = 1;
    const bool tscon_exited = wait_process(tscon_proc.process.h, 10000, &tscon_exit);
    tscon_proc.thread.reset();
    tscon_proc.process.reset();
    if (!tscon_exited) {
      BOOST_LOG(warning) << "[VIPLE-CONSOLE] tscon timed out session=" << id;
    }

    // 行程結束後 session id 可能晚一點才換。10 秒是等行程，這裡只再確認一下。
    const bool switched = wait_pred(2s, [&] {
      return WTSGetActiveConsoleSessionId() == id;
    });
    BOOST_LOG(info) << "[VIPLE-CONSOLE] tscon exit=" << tscon_exit << " exited=" << (tscon_exited ? 1 : 0) << " console=" << WTSGetActiveConsoleSessionId();
    if (!switched) {
      set_why(why, tscon_exited ? "console session did not switch" : "tscon timed out");
      return false;
    }

    // user_present 看的是 token，不是使用者名稱。剛接上時 token 可能還沒好。
    HANDLE user_token = nullptr;
    const bool got_token = wait_pred(3s, [&] {
      HANDLE got = nullptr;
      if (!WTSQueryUserToken(id, &got) || !got) {
        return false;
      }
      user_token = got;
      return true;
    });
    handle_t token {user_token};
    if (!lock_after) {
      if (!got_token) {
        BOOST_LOG(warning) << "[VIPLE-CONSOLE] session is on the console but WTSQueryUserToken is not ready session=" << id;
      }
      BOOST_LOG(warning) << "[VIPLE-CONSOLE] session=" << id << " is on the console without a lock; the physical display shows the desktop";
      return true;
    }
    if (!got_token) {
      BOOST_LOG(warning) << "[VIPLE-CONSOLE] WTSQueryUserToken failed after tscon session=" << id << " gle=" << GetLastError();
      disconnect_session(id, why);
      return false;
    }

    const std::wstring rundll = root + L"\\System32\\rundll32.exe";
    std::wstring lock_cmd = L"\"" + rundll + L"\" user32.dll,LockWorkStation";
    // lpDesktop 的型別是可寫指標。用區域緩衝，避免指到字面常數。
    wchar_t desktop[] = L"winsta0\\default";
    BOOST_LOG(info) << "[VIPLE-CONSOLE] locking session=" << id;
    spawned_t lock_proc;
    if (!spawn(rundll, std::move(lock_cmd), token.h, desktop, lock_proc, why, "lock")) {
      disconnect_session(id, why);
      return false;
    }
    DWORD lock_exit = 1;
    const bool lock_exited = wait_process(lock_proc.process.h, 10000, &lock_exit);
    lock_proc.thread.reset();
    lock_proc.process.reset();
    token.reset();
    BOOST_LOG(info) << "[VIPLE-CONSOLE] lock exit=" << lock_exit << " exited=" << (lock_exited ? 1 : 0);

    const bool locked = wait_pred(3s, [&] {
      return session_locked(id) && WTSGetActiveConsoleSessionId() == id;
    });
    if (!locked) {
      BOOST_LOG(warning) << "[VIPLE-CONSOLE] session=" << id << " did not report locked";
      disconnect_session(id, why);
      return false;
    }
    BOOST_LOG(info) << "[VIPLE-CONSOLE] session=" << id << " reclaimed and locked";
    return true;
  }
}  // namespace platf::console
