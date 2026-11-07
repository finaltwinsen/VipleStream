/**
 * @file src/platform/windows/vr_scene_focus_win.cpp
 * @brief §VR-FOCUS：把 SteamVR 場景程式的主視窗拉到前景（見 vr_scene_focus.h）。
 */
// clang-format off
#include <windows.h>
#include <tlhelp32.h>
// clang-format on

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "src/logging.h"
#include "src/vr/vr_scene_focus.h"

namespace vr::scene_focus {
  namespace {
    using namespace std::chrono_literals;

    struct find_ctx_t {
      DWORD pid;
      HWND best;
      LONG best_area;
    };

    BOOL CALLBACK enum_proc(HWND h, LPARAM lp) {
      auto *ctx = reinterpret_cast<find_ctx_t *>(lp);
      DWORD pid = 0;
      GetWindowThreadProcessId(h, &pid);
      if (pid != ctx->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) != nullptr) {
        return TRUE;
      }
      RECT r {};
      GetWindowRect(h, &r);
      const LONG area = std::max<LONG>(0, r.right - r.left) * std::max<LONG>(0, r.bottom - r.top);
      if (!ctx->best || area > ctx->best_area) {
        ctx->best = h;
        ctx->best_area = area;
      }
      return TRUE;
    }

    HWND main_window(DWORD pid) {
      find_ctx_t ctx {pid, nullptr, 0};
      EnumWindows(enum_proc, reinterpret_cast<LPARAM>(&ctx));
      return ctx.best;
    }

    std::string process_name(DWORD pid) {
      std::string name;
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
      if (snap == INVALID_HANDLE_VALUE) {
        return name;
      }
      PROCESSENTRY32W pe {sizeof(pe)};
      for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == pid) {
          for (const wchar_t *p = pe.szExeFile; *p; ++p) {
            name.push_back((char) std::tolower((unsigned char) (*p < 128 ? *p : '?')));
          }
          break;
        }
      }
      CloseHandle(snap);
      return name;
    }

    /// 前景是這些程式（或沒有前景）時才搶：都是串流時自己冒出來的，不會搶走 host 前有人在用的其他程式
    bool foreground_is_ours_to_take(DWORD fg_pid) {
      if (fg_pid == 0) {
        return true;
      }
      static const char *k_names[] = {
        "vrmonitor.exe", "vrserver.exe", "vrwebhelper.exe", "vrdashboard.exe", "vrcompositor.exe",
        "steam.exe", "steamwebhelper.exe", "explorer.exe", "viplestream-server.exe", "sunshine.exe",
      };
      const auto n = process_name(fg_pid);
      return std::any_of(std::begin(k_names), std::end(k_names), [&](const char *k) {
        return n == k;
      });
    }

    bool bring_to_front(HWND h) {
      const HWND fg = GetForegroundWindow();
      const DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
      const DWORD me = GetCurrentThreadId();
      // 前景鎖：先送一次 Alt（Windows 允許剛收到輸入的程式改前景），再掛上前景執行緒的輸入佇列
      INPUT in[2] {};
      in[0].type = INPUT_KEYBOARD;
      in[0].ki.wVk = VK_MENU;
      in[1] = in[0];
      in[1].ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(2, in, sizeof(INPUT));
      const bool attached = fg_tid && fg_tid != me && AttachThreadInput(me, fg_tid, TRUE);
      if (IsIconic(h)) {
        ShowWindow(h, SW_RESTORE);
      }
      SetForegroundWindow(h);
      if (attached) {
        AttachThreadInput(me, fg_tid, FALSE);
      }
      return GetForegroundWindow() == h;
    }

    class focus_t {
    public:
      explicit focus_t(std::function<uint32_t()> scene_pid):
          scene_pid_ {std::move(scene_pid)},
          th_ {[this]() {
            run();
          }} {
      }

      ~focus_t() {
        {
          std::lock_guard lk(mtx_);
          stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) {
          th_.join();
        }
      }

    private:
      void run() {
        DWORD last_pid = 0;
        int fixes = 0;  // 同一個場景程式被搶回前景的次數（避免和別的程式無限拉鋸）
        while (true) {
          {
            std::unique_lock lk(mtx_);
            if (cv_.wait_for(lk, 1s, [this]() {
                  return stop_;
                })) {
              return;
            }
          }
          const DWORD pid = scene_pid_ ? scene_pid_() : 0;
          if (pid != last_pid) {
            last_pid = pid;
            fixes = 0;
          }
          if (pid == 0 || fixes >= 20) {
            continue;
          }
          const HWND fg = GetForegroundWindow();
          DWORD fg_pid = 0;
          if (fg) {
            GetWindowThreadProcessId(fg, &fg_pid);
          }
          if (fg_pid == pid) {
            continue;
          }
          const HWND h = main_window(pid);
          if (!h || !foreground_is_ours_to_take(fg_pid)) {
            continue;  // 遊戲視窗還沒出來，或前景是別人在用的程式
          }
          const bool ok = bring_to_front(h);
          ++fixes;
          BOOST_LOG(info) << "[VIPLE-VR-FOCUS] scene app pid=" << pid << " was in the background (foreground "
                          << process_name(fg_pid) << " pid=" << fg_pid << "), brought to front: " << (ok ? "ok" : "failed")
                          << " (" << fixes << ')';
        }
      }

      std::function<uint32_t()> scene_pid_;
      std::mutex mtx_;
      std::condition_variable cv_;
      bool stop_ = false;
      std::thread th_;  // 最後初始化：建構子跑完其他成員才開始執行
    };
  }  // namespace

  std::shared_ptr<void> start(std::function<uint32_t()> scene_pid) {
    return std::make_shared<focus_t>(std::move(scene_pid));
  }
}  // namespace vr::scene_focus
