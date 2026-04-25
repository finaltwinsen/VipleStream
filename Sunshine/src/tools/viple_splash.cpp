/**
 * @file src/tools/viple_splash.cpp
 * @brief VipleStream 啟動遮罩 — topmost 全螢幕黑色視窗，覆蓋遊戲 loading 期
 *        的桌面露出。
 *
 * Sunshine 啟 Steam game 時呼叫 `steam://rungameid/<AppID>` URL handler。
 * 這個 URL handler 回得很快（steam.exe 已經在跑），Sunshine 立刻認為
 * 「launch 完成」進入串流狀態；但**實際的遊戲**還要 5-15 秒才會出現
 * （Steam overlay / DRM / shader comp / 首次 compile cache）。這段期間
 * client 看到的是 Windows 桌面——沒有隱私問題但體驗很差。
 *
 * 這支 exe 做的就一件事：spawn 後立刻蓋一層 topmost 全螢幕黑幕、等一段
 * 時間或直到偵測到有新的 fullscreen 視窗蓋上來（= 遊戲 ready），然後退
 * 出。Sunshine 把它當 `detached` command 在啟 Steam URL 前 spawn，
 * user session 裡跟 Steam 平行跑。
 *
 * CLI:
 *   viple-splash.exe [--timeout <seconds>] [--target-pid <PID>]
 *
 * --timeout N     最多顯示 N 秒後強制退出（default 10）
 * --target-pid P  如果指定且該 PID 出現 visible 主視窗，立刻退出
 *                 （未指定時純 timeout）
 *
 * 僅 Windows。C++17 + 純 Win32 GUI API。Timer/sleep/string 都走 C++
 * stdlib（chrono/thread/wstring_view）以避開 MinGW + `-static` 下的
 * CRT linking 問題（_wtoi、GetTickCount64 等 symbol 抓不到）。
 */

#ifdef _WIN32

  #define NOMINMAX
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellscalingapi.h>

  #include <chrono>
  #include <cstdint>
  #include <cstdlib>
  #include <string>
  #include <string_view>
  #include <thread>

namespace {

  using clock_t_ = std::chrono::steady_clock;

  // 解析 `--flag value` 形式的 CLI 參數。WinMain 拿到 lpCmdLine 是原始
  // 字串。用 std::wstring_view::find 避開 wcsstr（在 MinGW 靜態鏈結下
  // 會指向 msvcrt 的 import stub，跟我們的 link line 對不起來）。
  std::wstring_view find_arg(std::wstring_view cmdline, std::wstring_view flag) {
    auto pos = cmdline.find(flag);
    if (pos == std::wstring_view::npos) {
      return {};
    }
    auto after = cmdline.substr(pos + flag.size());
    // skip whitespace after the flag
    while (!after.empty() && (after.front() == L' ' || after.front() == L'\t')) {
      after.remove_prefix(1);
    }
    return after;
  }

  // Parse a non-negative int out of the leading digits of `s`. Stops at
  // first non-digit / whitespace. Returns -1 if no leading digit.
  long parse_leading_int(std::wstring_view s) {
    if (s.empty() || s.front() < L'0' || s.front() > L'9') {
      return -1;
    }
    // wcstol is part of C89/C++ <cwchar>; unlike _wtoi it's in the
    // standard runtime, so MinGW's libmingw32/libmsvcrt import lib
    // resolves it without extra tweaking.
    // Copy into null-terminated buffer so we can hand it to wcstol.
    std::wstring buf {s};
    wchar_t *end = nullptr;
    long v = std::wcstol(buf.c_str(), &end, 10);
    if (end == buf.c_str()) {
      return -1;
    }
    return v;
  }

  long long parse_leading_int64(std::wstring_view s) {
    if (s.empty() || s.front() < L'0' || s.front() > L'9') {
      return -1;
    }
    std::wstring buf {s};
    wchar_t *end = nullptr;
    long long v = std::wcstoll(buf.c_str(), &end, 10);
    if (end == buf.c_str()) {
      return -1;
    }
    return v;
  }

  struct SplashState {
    HWND               target_hwnd = nullptr;  // window we're waiting to be covered by
    DWORD              target_pid  = 0;        // 0 = no process filter (pure timeout)
    clock_t_::time_point start;
    std::chrono::milliseconds timeout {10000};
  };

  LRESULT CALLBACK splash_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC dc = reinterpret_cast<HDC>(wp);
        HBRUSH black = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        FillRect(dc, &rc, black);
        return 1;
      }
      case WM_CLOSE:
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
  }

  // Callback for EnumWindows: looks for a visible main window owned by
  // the target PID whose rect covers most of the display — that's the
  // signal "the game is up", we can kill the splash.
  BOOL CALLBACK enum_wnd_proc(HWND hwnd, LPARAM lparam) {
    auto *st = reinterpret_cast<SplashState *>(lparam);
    if (!IsWindowVisible(hwnd)) {
      return TRUE;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != st->target_pid) {
      return TRUE;
    }
    // Skip small tool windows / title-less dialogs (Steam splash etc).
    RECT r;
    if (!GetWindowRect(hwnd, &r)) {
      return TRUE;
    }
    int w = r.right - r.left;
    int h = r.bottom - r.top;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    // Must cover at least half the screen in each dim — filters Steam's
    // small "preparing to launch" dialogs. Games are typically fullscreen
    // or close to it.
    if (w < sw / 2 || h < sh / 2) {
      return TRUE;
    }
    st->target_hwnd = hwnd;
    return FALSE;  // stop enumerating
  }

}  // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int) {
  SplashState st;
  st.start = clock_t_::now();

  std::wstring_view cmdline {lpCmdLine ? lpCmdLine : L""};

  if (auto v = find_arg(cmdline, L"--timeout"); !v.empty()) {
    long n = parse_leading_int(v);
    if (n > 0 && n < 120) {
      st.timeout = std::chrono::milliseconds {n * 1000};
    }
  }
  if (auto v = find_arg(cmdline, L"--target-pid"); !v.empty()) {
    long long n = parse_leading_int64(v);
    if (n > 0) {
      st.target_pid = static_cast<DWORD>(n);
    }
  }

  // Best-effort per-monitor DPI awareness so the fullscreen sizing below
  // uses pixel dimensions (SM_CXSCREEN returns DIPs otherwise on HiDPI,
  // leaving a thin uncovered strip on the right/bottom).
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  WNDCLASSEXW wc {};
  wc.cbSize        = sizeof(wc);
  wc.lpfnWndProc   = splash_wnd_proc;
  wc.hInstance     = hInst;
  wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = L"VipleSplashWindow";
  if (!RegisterClassExW(&wc)) {
    return 1;
  }

  // Cover ALL monitors in the virtual desktop (VirtualScreen) rather
  // than just the primary — host may have VDD (primary) + physical
  // monitor both visible, we want both hidden during load.
  int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
  int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
  int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
  int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

  HWND hwnd = CreateWindowExW(
    WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
    L"VipleSplashWindow",
    L"VipleStream Loading",
    WS_POPUP,
    vx, vy, vw, vh,
    nullptr, nullptr, hInst, nullptr
  );
  if (!hwnd) {
    return 2;
  }
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);
  UpdateWindow(hwnd);

  // Main loop: pump messages, periodically check target window status
  // and timeout. 50ms polling is plenty — no user input goes through
  // this window (WS_EX_NOACTIVATE + WS_EX_TOOLWINDOW prevent focus).
  for (;;) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if (msg.message == WM_QUIT) {
        return 0;
      }
    }

    auto elapsed = clock_t_::now() - st.start;
    if (elapsed >= st.timeout) {
      break;
    }

    if (st.target_pid != 0) {
      st.target_hwnd = nullptr;
      EnumWindows(enum_wnd_proc, reinterpret_cast<LPARAM>(&st));
      if (st.target_hwnd) {
        // Wait a beat so the window actually renders something before
        // we uncover it — prevents "flash of black/initializing UI"
        // visible to the client.
        std::this_thread::sleep_for(std::chrono::milliseconds {250});
        break;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds {50});
  }

  DestroyWindow(hwnd);
  UnregisterClassW(L"VipleSplashWindow", hInst);
  return 0;
}

#endif  // _WIN32
