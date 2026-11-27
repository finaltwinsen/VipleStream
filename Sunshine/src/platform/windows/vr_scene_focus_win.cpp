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
#include <functional>
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
      focus_t(std::function<uint32_t()> scene_pid, std::function<std::vector<uint32_t>()> launched_pids):
          scene_pid_ {std::move(scene_pid)},
          launched_pids_ {std::move(launched_pids)},
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
        uint32_t idle_ticks = 0;  // 沒有場景程式的拍數
        uint32_t ticks = 0;
        // §REVIEW-1127 vr #5：launched_pids_() 每次對系統上每個行程做 OpenProcess＋QueryFullProcessImageNameW，
        // §VR-PADMODE 的非 VR 遊戲整場都沒有場景 pid，原本整場每 2 秒掃一次。改成：找到遊戲視窗後快取 pid／視窗，
        // 行程還活著、視窗還在就直接用，只每 10 秒重掃一次（啟動器先開、遊戲本體後開時仍能換到較大的視窗）；
        // 行程結束或視窗消失就立刻重掃。一直找不到時，前 60 秒照舊每 2 秒、之後放慢到 10 秒。
        DWORD cached_pid = 0;
        HWND cached_hwnd = nullptr;
        HANDLE cached_proc = nullptr;
        uint32_t ticks_since_found = 0;
        bool rescan_now = false;
        auto drop_cache = [&]() {
          if (cached_proc) {
            CloseHandle(cached_proc);
          }
          cached_proc = nullptr;
          cached_pid = 0;
          cached_hwnd = nullptr;
        };
        struct cache_closer_t {
          std::function<void()> f;

          ~cache_closer_t() {
            f();
          }
        } cache_closer {drop_cache};
        while (true) {
          {
            std::unique_lock lk(mtx_);
            if (cv_.wait_for(lk, 1s, [this]() {
                  return stop_;
                })) {
              return;
            }
          }
          DWORD pid = scene_pid_ ? scene_pid_() : 0;
          // 還沒有場景程式：看這一場啟動的遊戲有沒有開出視窗。有多個行程時挑視窗最大的那個
          bool launched = false;
          if (pid == 0 && launched_pids_) {
            ++idle_ticks;
            ++ticks_since_found;
            // §REVIEW-1127 vr #5：快取失效（行程結束或視窗消失）就丟掉並立刻重掃
            // （OpenProcess 拿不到 handle 時只看視窗，不要每拍都丟掉快取、退化成每秒全掃）
            if (cached_pid != 0 &&
                ((cached_proc && WaitForSingleObject(cached_proc, 0) != WAIT_TIMEOUT) || !IsWindow(cached_hwnd) || !IsWindowVisible(cached_hwnd))) {
              drop_cache();
              rescan_now = true;
            }
            const uint32_t scan_every = (cached_pid != 0 || ticks_since_found > 60) ? 10 : 2;
            if (rescan_now || (idle_ticks % scan_every) == 0) {
              rescan_now = false;
              LONG best_area = -1;
              HWND best_hwnd = nullptr;
              for (const auto p : launched_pids_()) {
                const HWND h = main_window(p);
                if (!h) {
                  continue;
                }
                RECT r {};
                GetWindowRect(h, &r);
                const LONG area = std::max<LONG>(0, r.right - r.left) * std::max<LONG>(0, r.bottom - r.top);
                if (area > best_area) {
                  best_area = area;
                  best_hwnd = h;
                  pid = p;
                  launched = true;
                }
              }
              if (launched) {
                if (pid != cached_pid) {
                  drop_cache();
                  cached_pid = pid;
                  cached_proc = OpenProcess(SYNCHRONIZE, FALSE, pid);  // 拿不到（nullptr）時只靠視窗判斷失效
                }
                cached_hwnd = best_hwnd;
                ticks_since_found = 0;
              } else {
                drop_cache();
              }
            } else if (cached_pid != 0 && (idle_ticks % 2) == 0) {
              // 照舊隔一拍才處理一次遊戲視窗（拉前景的頻率與次數上限跟修正前一樣）
              pid = cached_pid;
              launched = true;
            }
          }
          if (pid == 0 && last_pid != 0 && launched_pids_) {
            continue;  // 隔一拍才列一次行程：沒列的那一拍不要把 last_pid 與次數清掉
          }
          if (pid != last_pid) {
            last_pid = pid;
            fixes = 0;
          }
          if (pid == 0) {
            continue;
          }
          // §REVIEW-1111：原本拉了 20 次（成功或失敗都算）之後，這一場就再也不拉了——SteamVR 的狀態視窗或 Steam
          // 多搶幾次前景，遊戲之後就一直留在背景（Unreal 的遊戲會靜音）。改成：遊戲在前景時把次數歸零；
          // 連續 20 次都沒拉成功就放慢到 30 秒一次，不是永遠停掉。
          ++ticks;
          if (fixes >= 20 && ticks % 30 != 0) {
            continue;
          }
          const HWND fg = GetForegroundWindow();
          DWORD fg_pid = 0;
          if (fg) {
            GetWindowThreadProcessId(fg, &fg_pid);
          }
          const HWND h = main_window(pid);
          // 視窗縮到最小時，Windows 有時仍把它報成前景（全螢幕獨占的遊戲被搶走前景後自己縮小）：縮小著就一定要還原
          const bool iconic = h && IsIconic(h);
          if (fg_pid == pid && !iconic) {
            fixes = 0;
            continue;
          }
          if (!h || (!(iconic && fg_pid == pid) && !foreground_is_ours_to_take(fg_pid))) {
            continue;  // 遊戲視窗還沒出來，或前景是別人在用的程式
          }
          const bool ok = bring_to_front(h);
          ++fixes;
          BOOST_LOG(info) << "[VIPLE-VR-FOCUS] " << (launched ? "launched game (no VR scene yet)" : "scene app") << " pid=" << pid
                          << " was in the background (foreground "
                          << process_name(fg_pid) << " pid=" << fg_pid << "), brought to front: " << (ok ? "ok" : "failed")
                          << " (" << fixes << ')';
        }
      }

      std::function<uint32_t()> scene_pid_;
      std::function<std::vector<uint32_t>()> launched_pids_;
      std::mutex mtx_;
      std::condition_variable cv_;
      bool stop_ = false;
      std::thread th_;  // 最後初始化：建構子跑完其他成員才開始執行
    };
  }  // namespace

  std::shared_ptr<void> start(std::function<uint32_t()> scene_pid, std::function<std::vector<uint32_t>()> launched_pids) {
    return std::make_shared<focus_t>(std::move(scene_pid), std::move(launched_pids));
  }
}  // namespace vr::scene_focus
