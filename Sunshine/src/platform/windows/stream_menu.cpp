/**
 * @file src/platform/windows/stream_menu.cpp
 * @brief §STREAM-MENU：2D 串流期間，把 Windows 的電源選單換成串流控制選單（說明見 stream_menu.h）。
 *
 * 三個入口：
 *   1. 遠端左鍵按在開始選單的電源圖示（UI Automation 的 AutomationId == "PowerButton"）。
 *   2. 遠端在桌面（前景視窗類別 Progman／WorkerW）按 Alt+F4。
 *   3. 保險：串流期間出現「關閉 Windows」對話框（前兩個入口沒攔到時），取消它。
 *
 * 執行緒：
 *   - 輸入執行緒（呼叫 on_mouse_button／on_key）：只做便宜的檢查；UIA 查詢丟給專用執行緒、最多等 100 ms。
 *   - UIA 執行緒：常駐，持有 IUIAutomation。跨行程 COM 呼叫可能卡住，不能放在輸入執行緒上。
 *   - 對話框監看執行緒：常駐，串流期間每 300 ms 找一次「關閉 Windows」對話框。
 *   - 選單執行緒：每次顯示選單開一條，建立視窗、跑訊息迴圈、執行選到的動作後結束。
 *
 * 桌面：這幾條執行緒都不呼叫 SetThreadDesktop，留在行程的啟動桌面（winsta0\default）——開始選單、
 * 桌面與「關閉 Windows」對話框都只會出現在那裡；input.cpp 的 syncThreadDesktop 是每條執行緒各自的事。
 */
// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>

// lib includes
#include <boost/process/v1.hpp>

// prevent clang format from "optimizing" the header include order
// clang-format off
#include <WinSock2.h>
#include <Windows.h>
#include <oleauto.h>
#include <powrprof.h>
#include <uiautomation.h>
// clang-format on

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/nvhttp.h"
#include "src/platform/common.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/stream.h"
#include "src/vr/vr_session.h"
#include "stream_menu.h"
#include "utf_utils.h"

using namespace std::literals;

namespace platf::stream_menu {
  namespace {
    // ── 共用狀態 ────────────────────────────────────────────────────────────

    std::atomic<bool> g_streaming {false};  ///< 有串流 session（streaming_will_start～streaming_will_stop）
    std::atomic<bool> g_menu_open {false};  ///< 選單執行緒存在（同時間只會有一個選單）
    std::atomic<HWND> g_menu_hwnd {nullptr};

    std::atomic<bool> g_left_swallowed {false};  ///< 左鍵按下被吞，對應的放開也要吞
    std::atomic<std::uint64_t> g_f4_swallowed_ms {0};  ///< F4 按下被吞的時刻（0＝沒有）；重複與放開也要吞
    std::atomic<unsigned> g_alt_mask {0};  ///< 遠端按著的 Alt（bit0＝VK_MENU、bit1＝左、bit2＝右）

    std::mutex g_watch_mtx;
    std::condition_variable g_watch_cv;

    /// §STREAM-MENU：F4 的放開萬一沒送到（session 異常結束），不能讓 F4 永遠被吞——超過這個時間就當作沒吞過。
    constexpr std::uint64_t k_f4_swallow_max_ms = 10000;
    /// §STREAM-MENU：UIA 查詢最多讓輸入執行緒等這麼久（host 實測約 15 ms）；逾時當成沒攔到、照原樣注入。
    constexpr auto k_uia_timeout = 100ms;

    enum class source_e {
      power_button,
      alt_f4,
      shutdown_dialog,
    };

    enum action_e : int {
      act_disconnect = 0,
      act_quit_app,
      act_lock,
      act_sleep,
      act_shutdown,
      act_restart,
      act_cancel,
      act_count,
    };

    const char *action_name(int action) {
      switch (action) {
        case act_disconnect:
          return "disconnect";
        case act_quit_app:
          return "quit-app";
        case act_lock:
          return "lock";
        case act_sleep:
          return "sleep";
        case act_shutdown:
          return "shutdown";
        case act_restart:
          return "restart";
        default:
          return "cancel";
      }
    }

    /// 設定開著、而且有串流 session。是不是 VR 另外用 ::vr::active() 看（要拿鎖，留到便宜的檢查都過了才問）。
    bool armed() {
      return config::input.stream_menu && g_streaming.load(std::memory_order_relaxed);
    }

    // ── log 節流：同一種錯誤每分鐘最多一行 ─────────────────────────────────

    struct throttle_t {
      std::atomic<std::uint64_t> last_ms {0};
      std::atomic<std::uint32_t> suppressed {0};
    };

    /// @return true＝這次可以印；suppressed_out＝上次印之後被略過的次數
    bool throttle_pass(throttle_t &t, std::uint32_t &suppressed_out) {
      const std::uint64_t now = GetTickCount64();
      const std::uint64_t last = t.last_ms.load(std::memory_order_relaxed);
      if (last != 0 && now - last < 60000) {
        t.suppressed.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      t.last_ms.store(now ? now : 1, std::memory_order_relaxed);
      suppressed_out = t.suppressed.exchange(0, std::memory_order_relaxed);
      return true;
    }

    throttle_t g_log_uia_busy;
    throttle_t g_log_uia_timeout;
    throttle_t g_log_uia_call;

    // ── 行程映像檔名比對（以 pid 快取）──────────────────────────────────────

    /**
     * §STREAM-MENU：每次左鍵按下都會問「游標底下的視窗是不是開始選單的行程」，所以只記最近一個 pid 的答案：
     * 連續點同一個視窗時只剩一次 atomic 讀取。pid 被重用頂多多做一次 UIA 查詢或漏攔一次，不會誤動作
     * （真正的判斷是後面的 AutomationId／對話框控制項）。
     */
    struct pid_cache_t {
      std::atomic<std::uint64_t> v {0};  ///< (pid << 1) | 是否相符；0＝空
    };

    bool image_name_is(DWORD pid, const wchar_t *name, pid_cache_t &cache) {
      const std::uint64_t cached = cache.v.load(std::memory_order_relaxed);
      if (cached != 0 && (DWORD) (cached >> 1) == pid) {
        return (cached & 1) != 0;
      }

      bool match = false;
      if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t path[MAX_PATH * 2] {};
        DWORD len = (DWORD) (sizeof(path) / sizeof(path[0]));
        if (QueryFullProcessImageNameW(h, 0, path, &len)) {
          const wchar_t *base = std::wcsrchr(path, L'\\');
          base = base ? base + 1 : path;
          match = _wcsicmp(base, name) == 0;
        }
        CloseHandle(h);
      }
      cache.v.store(((std::uint64_t) pid << 1) | (match ? 1 : 0), std::memory_order_relaxed);
      return match;
    }

    pid_cache_t g_start_menu_pid;
    pid_cache_t g_explorer_pid;

    // ── UI Automation 查詢（專用執行緒）────────────────────────────────────

    // §STREAM-MENU：GUID 自己帶，不依賴 MinGW 的 libuuid 有沒有收錄這幾個符號（header 只有 extern 宣告）。
    const GUID k_clsid_uia8 = {0xe22ad333, 0xb25f, 0x460c, {0x83, 0xd0, 0x05, 0x81, 0x10, 0x73, 0x95, 0xc9}};  // CUIAutomation8
    const GUID k_clsid_uia = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};  // CUIAutomation
    const GUID k_iid_uia = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};  // IUIAutomation
    const GUID k_iid_uia2 = {0x34723aff, 0x0c9d, 0x49d0, {0x98, 0x96, 0x7a, 0xb5, 0x2d, 0xf8, 0xcd, 0x8a}};  // IUIAutomation2

    enum class probe_e {
      hit,  ///< 游標底下是電源圖示
      miss,  ///< 是別的元素
      unknown,  ///< 查不出來（UIA 不能用、逾時、呼叫失敗）——一律當成沒攔到
    };

    class uia_worker_t {
    public:
      /// 啟動執行緒並建立 IUIAutomation（第一次建立要載入 DLL，可能超過 100 ms，所以在 session 開始時先做）
      void start() {
        std::lock_guard lk {mtx_};
        start_locked();
      }

      /**
       * @brief 問游標底下是不是電源圖示；最多等 timeout。
       * 上一次查詢還卡著（已經逾時過）時直接回 unknown，不排隊——輸入執行緒不能被拖住。
       */
      probe_e probe(POINT pt, std::chrono::milliseconds timeout) {
        std::unique_lock lk {mtx_};
        start_locked();
        if (dead_) {
          return probe_e::unknown;
        }
        if (busy_) {
          std::uint32_t suppressed = 0;
          if (throttle_pass(g_log_uia_busy, suppressed)) {
            BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] uia: previous query is still running - click passed through (suppressed="sv << suppressed << ')';
          }
          return probe_e::unknown;
        }

        pt_ = pt;
        const std::uint64_t seq = ++req_seq_;
        busy_ = true;
        pending_ = true;
        cv_req_.notify_one();

        if (!cv_done_.wait_for(lk, timeout, [&] {
              return done_seq_ == seq;
            })) {
          std::uint32_t suppressed = 0;
          if (throttle_pass(g_log_uia_timeout, suppressed)) {
            BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] uia: query timed out after "sv << timeout.count() << " ms - click passed through (suppressed="sv << suppressed << ')';
          }
          return probe_e::unknown;
        }
        return result_;
      }

    private:
      void start_locked() {
        if (started_) {
          return;
        }
        started_ = true;
        try {
          std::thread([this] {
            run();
          }).detach();
        } catch (const std::exception &e) {
          dead_ = true;
          BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] uia: could not start the worker thread ("sv << e.what() << ") - the power button will not be intercepted"sv;
        }
      }

      void run() {
        IUIAutomation *uia = nullptr;
        const char *flavor = "CUIAutomation8";

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_ok = SUCCEEDED(hr);
        if (com_ok) {
          hr = CoCreateInstance(k_clsid_uia8, nullptr, CLSCTX_INPROC_SERVER, k_iid_uia, (void **) &uia);
          if (FAILED(hr) || !uia) {
            uia = nullptr;
            flavor = "CUIAutomation";
            hr = CoCreateInstance(k_clsid_uia, nullptr, CLSCTX_INPROC_SERVER, k_iid_uia, (void **) &uia);
          }
        }

        if (!com_ok || FAILED(hr) || !uia) {
          BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] uia: unavailable (hr=0x"sv << util::hex((std::uint32_t) hr).to_string_view()
                             << ") - the power button will not be intercepted"sv;
          if (com_ok) {
            CoUninitialize();
          }
          std::lock_guard lk {mtx_};
          dead_ = true;
          busy_ = false;
          result_ = probe_e::unknown;
          done_seq_ = req_seq_;
          cv_done_.notify_all();
          return;
        }

        // §STREAM-MENU：目標行程沒回應時，預設的逾時是好幾秒到幾十秒；縮短，讓卡住的查詢早點回來、
        // 下一次點擊才有機會再查（輸入執行緒自己只等 k_uia_timeout）。
        {
          IUIAutomation2 *uia2 = nullptr;
          if (SUCCEEDED(uia->QueryInterface(k_iid_uia2, (void **) &uia2)) && uia2) {
            uia2->put_ConnectionTimeout(1000);
            uia2->put_TransactionTimeout(1000);
            uia2->Release();
          }
        }
        BOOST_LOG(info) << "[VIPLE-STREAM-MENU] uia: ready ("sv << flavor << ')';

        std::unique_lock lk {mtx_};
        for (;;) {
          cv_req_.wait(lk, [&] {
            return pending_;
          });
          pending_ = false;
          const POINT pt = pt_;
          const std::uint64_t seq = req_seq_;
          lk.unlock();

          const auto t0 = std::chrono::steady_clock::now();
          const probe_e r = query(uia, pt);
          const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
          BOOST_LOG(debug) << "[VIPLE-STREAM-MENU] uia: ElementFromPoint("sv << pt.x << ',' << pt.y << ") -> "sv
                           << (r == probe_e::hit ? "PowerButton"sv : r == probe_e::miss ? "other"sv : "failed"sv) << " in "sv << ms << " ms"sv;

          lk.lock();
          result_ = r;
          done_seq_ = seq;
          busy_ = false;
          cv_done_.notify_all();
        }
      }

      static probe_e query(IUIAutomation *uia, POINT pt) {
        IUIAutomationElement *element = nullptr;
        HRESULT hr = uia->ElementFromPoint(pt, &element);
        BSTR id = nullptr;
        if (SUCCEEDED(hr) && element) {
          hr = element->get_CurrentAutomationId(&id);
        } else if (SUCCEEDED(hr)) {
          hr = E_POINTER;
        }

        probe_e r = probe_e::unknown;
        if (SUCCEEDED(hr)) {
          r = (id && std::wcscmp(id, L"PowerButton") == 0) ? probe_e::hit : probe_e::miss;
        } else {
          std::uint32_t suppressed = 0;
          if (throttle_pass(g_log_uia_call, suppressed)) {
            BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] uia: query failed (hr=0x"sv << util::hex((std::uint32_t) hr).to_string_view()
                               << ") - click passed through (suppressed="sv << suppressed << ')';
          }
        }

        if (id) {
          SysFreeString(id);
        }
        if (element) {
          element->Release();
        }
        return r;
      }

      std::mutex mtx_;
      std::condition_variable cv_req_;
      std::condition_variable cv_done_;
      bool started_ = false;
      bool dead_ = false;  ///< UIA 建不起來；之後一律回 unknown
      bool pending_ = false;  ///< 有還沒被執行緒取走的請求
      bool busy_ = false;  ///< 請求送出後、結果回來前
      std::uint64_t req_seq_ = 0;
      std::uint64_t done_seq_ = 0;
      POINT pt_ {};
      probe_e result_ = probe_e::unknown;
    };

    /// 永不解構：常駐執行緒在行程結束時還在用它（避免 static 解構順序的問題）
    uia_worker_t &uia_worker() {
      static auto *w = new uia_worker_t;
      return *w;
    }

    // ── 選單視窗 ────────────────────────────────────────────────────────────

    // 96 DPI 下的尺寸（像素）
    constexpr int k_menu_w = 400;
    constexpr int k_pad = 8;
    constexpr int k_title_h = 44;
    constexpr int k_item_h = 40;
    constexpr int k_sep_h = 9;  ///< 「取消」上方的分隔
    constexpr UINT_PTR k_fg_timer = 1;

    // §STREAM-MENU：中文用 \u 寫，不依賴編譯器對原始檔編碼的假設。
    const wchar_t *const k_text_en[act_count + 1] = {
      L"Disconnect stream (host and app keep running)",
      L"Quit app and disconnect",
      L"Lock",
      L"Sleep",
      L"Shut down",
      L"Restart",
      L"Cancel",
      L"Stream menu",  // 標題
    };
    const wchar_t *const k_text_zh[act_count + 1] = {
      L"\u4e2d\u65b7\u4e32\u6d41\uff08\u4e3b\u6a5f\u8207 App \u4fdd\u7559\uff09",  // 中斷串流（主機與 App 保留）
      L"\u7d50\u675f App \u4e26\u4e2d\u65b7\u4e32\u6d41",  // 結束 App 並中斷串流
      L"\u9396\u5b9a",  // 鎖定
      L"\u7761\u7720",  // 睡眠
      L"\u95dc\u6a5f",  // 關機
      L"\u91cd\u65b0\u555f\u52d5",  // 重新啟動
      L"\u53d6\u6d88",  // 取消
      L"\u4e32\u6d41\u63a7\u5236",  // 串流控制
    };

    struct menu_t {
      const wchar_t *const *text = k_text_en;
      int dpi = 96;
      int sel = 0;  ///< 目前選取的項目（鍵盤與滑鼠共用）
      int pressed = -1;  ///< 左鍵按下時所在的項目；同一項放開才算選取
      int result = act_cancel;
      int fg_retries = 0;
      HFONT font = nullptr;
      HFONT title_font = nullptr;
    };

    int px(const menu_t &m, int v) {
      return MulDiv(v, m.dpi, 96);
    }

    SIZE menu_size(const menu_t &m) {
      return SIZE {px(m, k_menu_w), px(m, k_pad) * 2 + px(m, k_title_h) + px(m, k_item_h) * act_count + px(m, k_sep_h)};
    }

    RECT item_rect(const menu_t &m, int i) {
      RECT r;
      r.left = px(m, k_pad);
      r.right = px(m, k_menu_w) - px(m, k_pad);
      r.top = px(m, k_pad) + px(m, k_title_h) + px(m, k_item_h) * i + (i >= act_cancel ? px(m, k_sep_h) : 0);
      r.bottom = r.top + px(m, k_item_h);
      return r;
    }

    int hit_test(const menu_t &m, int x, int y) {
      const POINT pt {x, y};
      for (int i = 0; i < act_count; i++) {
        const RECT r = item_rect(m, i);
        if (PtInRect(&r, pt)) {
          return i;
        }
      }
      return -1;
    }

    void fill(HDC dc, const RECT &r, COLORREF color) {
      SetBkColor(dc, color);
      ExtTextOutW(dc, 0, 0, ETO_OPAQUE, &r, nullptr, 0, nullptr);
    }

    void paint(HWND hwnd, const menu_t &m) {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT rc;
      GetClientRect(hwnd, &rc);

      // 先畫到記憶體再一次貼上，選取列移動時不會閃
      HDC dc = CreateCompatibleDC(hdc);
      HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
      HGDIOBJ old_bmp = SelectObject(dc, bmp);

      fill(dc, rc, RGB(96, 96, 96));  // 邊框
      RECT inner {1, 1, rc.right - 1, rc.bottom - 1};
      fill(dc, inner, RGB(32, 32, 32));

      SetBkMode(dc, TRANSPARENT);
      const UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX;

      HGDIOBJ old_font = SelectObject(dc, m.title_font ? m.title_font : GetStockObject(DEFAULT_GUI_FONT));
      RECT title {px(m, k_pad) + px(m, 12), px(m, k_pad), rc.right - px(m, k_pad), px(m, k_pad) + px(m, k_title_h)};
      SetTextColor(dc, RGB(255, 255, 255));
      DrawTextW(dc, m.text[act_count], -1, &title, fmt);

      SelectObject(dc, m.font ? m.font : GetStockObject(DEFAULT_GUI_FONT));
      for (int i = 0; i < act_count; i++) {
        RECT r = item_rect(m, i);
        if (i == act_cancel) {
          RECT sep {r.left, r.top - px(m, k_sep_h) / 2 - 1, r.right, r.top - px(m, k_sep_h) / 2};
          fill(dc, sep, RGB(80, 80, 80));
        }
        if (i == m.sel) {
          fill(dc, r, RGB(0, 103, 192));
        }
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(240, 240, 240));
        r.left += px(m, 12);
        DrawTextW(dc, m.text[i], -1, &r, fmt);
      }

      BitBlt(hdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
      SelectObject(dc, old_font);
      SelectObject(dc, old_bmp);
      DeleteObject(bmp);
      DeleteDC(dc);
      EndPaint(hwnd, &ps);
    }

    /**
     * §STREAM-MENU：把選單帶到前景，鍵盤才會送到這裡。server 不是使用者剛才操作的行程，SetForegroundWindow
     * 可能被系統拒絕；被拒時暫時接上目前前景執行緒的輸入佇列再試一次。真的拿不到也沒關係——
     * 視窗是置頂的，滑鼠照樣能點。
     */
    void force_foreground(HWND hwnd) {
      if (GetForegroundWindow() == hwnd) {
        return;
      }
      if (SetForegroundWindow(hwnd) && GetForegroundWindow() == hwnd) {
        return;
      }
      const HWND fg = GetForegroundWindow();
      const DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
      const DWORD my_tid = GetCurrentThreadId();
      if (fg_tid && fg_tid != my_tid && AttachThreadInput(my_tid, fg_tid, TRUE)) {
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        AttachThreadInput(my_tid, fg_tid, FALSE);
      }
    }

    void finish(HWND hwnd, menu_t &m, int result) {
      m.result = result;
      DestroyWindow(hwnd);
    }

    /// 鍵盤操作；Alt 還按著時（Alt+F4 進來的）按鍵會以 WM_SYSKEYDOWN 送來，兩種訊息都走這裡
    bool handle_key(HWND hwnd, menu_t &m, WPARAM vk) {
      switch (vk) {
        case VK_UP:
          m.sel = (m.sel + act_count - 1) % act_count;
          break;
        case VK_DOWN:
        case VK_TAB:
          m.sel = (m.sel + 1) % act_count;
          break;
        case VK_HOME:
          m.sel = 0;
          break;
        case VK_END:
          m.sel = act_count - 1;
          break;
        case VK_RETURN:
        case VK_SPACE:
          finish(hwnd, m, m.sel);
          return true;
        case VK_ESCAPE:
          finish(hwnd, m, act_cancel);
          return true;
        default:
          return false;
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return true;
    }

    LRESULT CALLBACK menu_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
      if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR) ((CREATESTRUCTW *) lp)->lpCreateParams);
        return DefWindowProcW(hwnd, msg, wp, lp);
      }
      auto *m = (menu_t *) GetWindowLongPtrW(hwnd, GWLP_USERDATA);
      if (!m) {
        return DefWindowProcW(hwnd, msg, wp, lp);
      }

      switch (msg) {
        case WM_ERASEBKGND:
          return 1;
        case WM_PAINT:
          paint(hwnd, *m);
          return 0;
        case WM_MOUSEMOVE:
          {
            const int i = hit_test(*m, (short) LOWORD(lp), (short) HIWORD(lp));
            if (i >= 0 && i != m->sel) {
              m->sel = i;
              InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
          }
        case WM_LBUTTONDOWN:
          m->pressed = hit_test(*m, (short) LOWORD(lp), (short) HIWORD(lp));
          if (m->pressed >= 0) {
            m->sel = m->pressed;
            InvalidateRect(hwnd, nullptr, FALSE);
          }
          return 0;
        case WM_LBUTTONUP:
          {
            // 要在同一項按下又放開才算：攔到電源圖示那一下的「放開」可能落在剛出現的選單上
            const int i = hit_test(*m, (short) LOWORD(lp), (short) HIWORD(lp));
            const int pressed = m->pressed;
            m->pressed = -1;
            if (i >= 0 && i == pressed) {
              finish(hwnd, *m, i);
            }
            return 0;
          }
        case WM_KEYDOWN:
          if (handle_key(hwnd, *m, wp)) {
            return 0;
          }
          break;
        case WM_SYSKEYDOWN:
          // Alt+F4 的重複不可以把剛顯示的選單關掉；其餘按鍵照一般按鍵處理
          if (wp == VK_F4 || handle_key(hwnd, *m, wp)) {
            return 0;
          }
          break;
        case WM_SYSCOMMAND:
          // 放開 Alt 會進入選單列模式（這個視窗沒有選單列），吃掉
          if ((wp & 0xFFF0) == SC_KEYMENU) {
            return 0;
          }
          break;
        case WM_TIMER:
          if (wp == k_fg_timer) {
            // 開始選單收起來時前景會被還給原本的視窗，所以顯示後再搶幾次
            force_foreground(hwnd);
            if (++m->fg_retries >= 5 || GetForegroundWindow() == hwnd) {
              KillTimer(hwnd, k_fg_timer);
            }
            return 0;
          }
          break;
        case WM_CLOSE:
          finish(hwnd, *m, act_cancel);
          return 0;
        case WM_DESTROY:
          KillTimer(hwnd, k_fg_timer);
          PostQuitMessage(0);
          return 0;
        default:
          break;
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    }

    int window_dpi(HWND hwnd) {
      // GetDpiForWindow 是 Windows 10 1607 才有，動態取（同 display_base.cpp 對 DPI API 的做法）
      using fn_t = UINT(WINAPI *)(HWND);
      static const auto fn = (fn_t) (void *) GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
      const UINT dpi = fn ? fn(hwnd) : 0;
      return dpi ? (int) dpi : 96;
    }

    /// 顯示選單並等使用者選；回傳 action_e（建不起視窗、串流已結束都回 act_cancel）
    int run_menu(source_e source) {
      if (source != source_e::alt_f4) {
        // 等開始選單／對話框收起來，免得它們關閉時把前景搶回去
        Sleep(150);
      }

      const HINSTANCE inst = GetModuleHandleW(nullptr);
      static const wchar_t k_class[] = L"VipleStreamMenu";
      WNDCLASSEXW wc {};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = menu_wndproc;
      wc.hInstance = inst;
      wc.hCursor = LoadCursorW(nullptr, (LPCWSTR) IDC_ARROW);
      wc.lpszClassName = k_class;
      if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] menu: RegisterClassEx failed ("sv << GetLastError() << ')';
        return act_cancel;
      }

      menu_t m;
      // §STREAM-MENU：介面只分中文（繁體）與英文兩種，照系統 UI 語言選
      const bool zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
      m.text = zh ? k_text_zh : k_text_en;

      // 放在游標所在的螢幕（遠端使用者正在看的那一個）
      POINT cursor {};
      GetCursorPos(&cursor);
      MONITORINFO mi {};
      mi.cbSize = sizeof(mi);
      if (!GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        mi.rcWork = RECT {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
      }

      // 先在目標螢幕上建立（隱藏），才問得到那個螢幕的 DPI
      HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, k_class, m.text[act_count], WS_POPUP, mi.rcWork.left, mi.rcWork.top, 16, 16, nullptr, nullptr, inst, &m);
      if (!hwnd) {
        BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] menu: CreateWindowEx failed ("sv << GetLastError() << ')';
        return act_cancel;
      }

      m.dpi = window_dpi(hwnd);
      const wchar_t *face = zh ? L"Microsoft JhengHei UI" : L"Segoe UI";
      m.font = CreateFontW(-MulDiv(11, m.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
      m.title_font = CreateFontW(-MulDiv(12, m.dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);

      const SIZE size = menu_size(m);
      const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - size.cx) / 2;
      const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - size.cy) / 2;
      SetWindowPos(hwnd, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE);

      g_menu_hwnd.store(hwnd);
      if (!g_streaming.load()) {
        // 建立視窗的這段時間串流結束了，session_stopped 沒看到視窗——自己收掉
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
      } else {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
        force_foreground(hwnd);
        SetTimer(hwnd, k_fg_timer, 150, nullptr);
      }

      MSG msg;
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }

      g_menu_hwnd.store(nullptr);
      if (m.font) {
        DeleteObject(m.font);
      }
      if (m.title_font) {
        DeleteObject(m.title_font);
      }
      return m.result;
    }

    // ── 動作 ────────────────────────────────────────────────────────────────

    std::string system32_exe(const wchar_t *exe) {
      // §STREAM-MENU：一律用完整路徑，不靠 PATH 找（server 以 SYSTEM 執行，不可以被 PATH 上的同名程式頂替）
      wchar_t dir[MAX_PATH] {};
      const UINT n = GetSystemDirectoryW(dir, MAX_PATH);
      std::wstring path = (n > 0 && n < MAX_PATH) ? std::wstring {dir} : std::wstring {L"C:\\Windows\\System32"};
      path += L'\\';
      path += exe;
      return '"' + utf_utils::to_utf8(path) + '"';
    }

    /**
     * §STREAM-MENU：server 在使用者的工作階段裡以 SYSTEM 執行。鎖定、關機、重新啟動都交給「登入的使用者」
     * 身分的系統程式做（platf::run_command 的 elevated=false：SYSTEM 時用主控台使用者的 token
     * CreateProcessAsUser）——和使用者自己從開始選單做是同一個身分、同樣的權限；沒有人登入時會失敗並記下來。
     */
    bool run_as_user(const std::string &cmd, std::string &error) {
      std::error_code ec;
      boost::filesystem::path working_dir;
      boost::process::v1::environment env = boost::this_process::environment();
      auto child = platf::run_command(false, false, cmd, working_dir, env, nullptr, ec, nullptr);
      if (ec) {
        error = ec.message();
        return false;
      }
      child.detach();
      return true;
    }

    bool enable_shutdown_privilege() {
      HANDLE token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
      }
      TOKEN_PRIVILEGES tp {};
      tp.PrivilegeCount = 1;
      tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
      bool ok = LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &tp.Privileges[0].Luid) &&
                AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) &&
                GetLastError() == ERROR_SUCCESS;
      CloseHandle(token);
      return ok;
    }

    void log_result(int action, bool ok, const std::string &detail = {}) {
      if (ok) {
        BOOST_LOG(info) << "[VIPLE-STREAM-MENU] action "sv << action_name(action) << ": ok"sv << (detail.empty() ? ""s : " ("s + detail + ')');
      } else {
        BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] action "sv << action_name(action) << ": failed"sv << (detail.empty() ? ""s : " ("s + detail + ')');
      }
    }

    void perform(int action) {
      std::string error;
      switch (action) {
        case act_disconnect:
          // 和 web UI 停用 client 時用的是同一個函式：收掉所有串流 session，App 不動
          // 先讓控制迴圈送出正常結束的通知，client 才會馬上乾淨地結束（不送的話它要等約 38 秒逾時、跳錯誤）
          stream::request_graceful_termination_notice();
          std::this_thread::sleep_for(400ms);
          rtsp_stream::terminate_sessions();
          log_result(action, true, "app left running"s);
          break;

        case act_quit_app:
          {
            // 和 client 的「結束 App」（/cancel）同樣的順序：先請 Steam 遊戲正常關閉（背景執行、不是 Steam 遊戲時不做事），
            // 再走 web UI「Force Disconnect」那條現成路徑（收 session、作廢 VR 預約、結束 App、還原顯示設定）。
            proc::proc.quit_steam_game();
            const bool had_app = nvhttp::force_cancel_current_session();
            if (!had_app) {
              // 沒有 App 在跑（例如已經自己結束）：至少把串流收掉
              rtsp_stream::terminate_sessions();
            }
            log_result(action, true, had_app ? "app terminated"s : "no app was running"s);
            break;
          }

        case act_lock:
          {
            const bool ok = run_as_user(system32_exe(L"rundll32.exe") + " user32.dll,LockWorkStation", error);
            log_result(action, ok, error);
            break;
          }

        case act_sleep:
          {
            // SetSuspendState 要 SeShutdownPrivilege；SYSTEM 有這個特權但預設沒啟用。
            // 呼叫會等到系統喚醒才回來，所以先記一行。
            const bool priv = enable_shutdown_privilege();
            BOOST_LOG(info) << "[VIPLE-STREAM-MENU] action sleep: suspending (privilege "sv << (priv ? "enabled"sv : "NOT enabled"sv) << ')';
            const bool ok = SetSuspendState(FALSE, FALSE, FALSE) != 0;
            log_result(action, ok, ok ? "resumed"s : "SetSuspendState error "s + std::to_string(GetLastError()));
            break;
          }

        case act_shutdown:
        case act_restart:
          {
            // 不帶 /f：有未儲存的工作時由 Windows 照常詢問，不強制關掉使用者的程式
            const bool ok = run_as_user(system32_exe(L"shutdown.exe") + (action == act_shutdown ? " /s /t 0" : " /r /t 0"), error);
            log_result(action, ok, error);
            break;
          }

        default:
          break;
      }
    }

    const char *source_name(source_e source) {
      switch (source) {
        case source_e::power_button:
          return "power-button (start menu)";
        case source_e::alt_f4:
          return "alt-f4 (desktop)";
        default:
          return "shutdown-dialog (cancelled)";
      }
    }

    /// 記下攔到的入口並顯示選單；已經有選單時只記不重開
    void show_menu(source_e source) {
      if (g_menu_open.exchange(true)) {
        BOOST_LOG(info) << "[VIPLE-STREAM-MENU] intercepted: "sv << source_name(source) << " - menu is already open"sv;
        return;
      }
      BOOST_LOG(info) << "[VIPLE-STREAM-MENU] intercepted: "sv << source_name(source);

      try {
        std::thread([source] {
          const int action = run_menu(source);
          // 動作可能很久才回來（睡眠要到喚醒）；先放掉旗標，之後還能再開選單
          g_menu_open.store(false);
          BOOST_LOG(info) << "[VIPLE-STREAM-MENU] selected: "sv << action_name(action);
          perform(action);
        }).detach();
      } catch (const std::exception &e) {
        g_menu_open.store(false);
        BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] menu: could not start the menu thread ("sv << e.what() << ')';
      }
    }

    // ── 保險：「關閉 Windows」對話框 ────────────────────────────────────────

    /**
     * §STREAM-MENU：explorer.exe 的「關閉 Windows」對話框（桌面 Alt+F4）。標題跟著語系變，所以認控制項 ID：
     * ComboBox 2001、Static 2027／2028／2002、確定 1、取消 2（2026-11-19 在 Windows 11 build 26300 實測）。
     */
    bool is_shutdown_dialog(HWND hwnd) {
      if (!IsWindowVisible(hwnd)) {
        return false;
      }
      for (int id : {2001, 2027, 2028, 2002, (int) IDOK, (int) IDCANCEL}) {
        if (!GetDlgItem(hwnd, id)) {
          return false;
        }
      }
      wchar_t cls[32] {};
      GetClassNameW(GetDlgItem(hwnd, 2001), cls, 32);
      if (_wcsicmp(cls, L"ComboBox") != 0) {
        return false;
      }
      DWORD pid = 0;
      GetWindowThreadProcessId(hwnd, &pid);
      return pid && image_name_is(pid, L"explorer.exe", g_explorer_pid);
    }

    void check_shutdown_dialog() {
      // 只有監看執行緒會用到
      static HWND last_hwnd = nullptr;
      static std::uint64_t last_ms = 0;
      static bool escalated = false;

      HWND hwnd = nullptr;
      while ((hwnd = FindWindowExW(nullptr, hwnd, L"#32770", nullptr)) != nullptr) {
        if (!is_shutdown_dialog(hwnd)) {
          continue;
        }
        const std::uint64_t now = GetTickCount64();
        if (hwnd == last_hwnd) {
          // 已經送過取消。兩秒後還在就改送 WM_CLOSE，只試一次、只記一次
          if (!escalated && now - last_ms >= 2000) {
            escalated = true;
            BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] shutdown-dialog: still open 2 s after IDCANCEL - sending WM_CLOSE"sv;
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
          }
          return;
        }
        last_hwnd = hwnd;
        last_ms = now;
        escalated = false;

        // 和使用者按「取消」一樣
        const BOOL posted = PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), (LPARAM) GetDlgItem(hwnd, IDCANCEL));
        if (!posted) {
          BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] shutdown-dialog: could not post IDCANCEL ("sv << GetLastError() << ')';
        }
        show_menu(source_e::shutdown_dialog);
        return;
      }
      last_hwnd = nullptr;
    }

    void watch_thread() {
      for (;;) {
        {
          std::unique_lock lk {g_watch_mtx};
          g_watch_cv.wait(lk, [] {
            return g_streaming.load();
          });
        }
        // §STREAM-MENU：對話框從出現到使用者按下確定至少要幾百毫秒，300 ms 輪詢夠用，而且沒有 WinEvent hook
        // 那些「要有訊息迴圈、hook 跟著執行緒桌面」的條件。沒有串流時整條執行緒睡在上面的條件變數。
        while (g_streaming.load()) {
          if (config::input.stream_menu && !::vr::active()) {
            check_shutdown_dialog();
          }
          std::this_thread::sleep_for(300ms);
        }
      }
    }

    void start_watch_thread() {
      static std::once_flag once;
      std::call_once(once, [] {
        try {
          std::thread(watch_thread).detach();
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "[VIPLE-STREAM-MENU] shutdown-dialog: could not start the watch thread ("sv << e.what() << ')';
        }
      });
    }

    bool foreground_is_desktop() {
      const HWND fg = GetForegroundWindow();
      if (!fg) {
        return false;
      }
      wchar_t cls[32] {};
      if (!GetClassNameW(fg, cls, 32)) {
        return false;
      }
      return std::wcscmp(cls, L"Progman") == 0 || std::wcscmp(cls, L"WorkerW") == 0;
    }

    void send_escape() {
      INPUT in[2] {};
      in[0].type = INPUT_KEYBOARD;
      in[0].ki.wVk = VK_ESCAPE;
      in[1] = in[0];
      in[1].ki.dwFlags = KEYEVENTF_KEYUP;
      SendInput(2, in, sizeof(INPUT));
    }
  }  // namespace

  void session_started() {
    g_left_swallowed.store(false);
    g_f4_swallowed_ms.store(0);
    g_alt_mask.store(0);
    {
      std::lock_guard lk {g_watch_mtx};
      g_streaming.store(true);
    }
    if (config::input.stream_menu) {
      // UIA 第一次建立要載入 DLL；現在先做，第一次點電源圖示才不會因為逾時漏攔
      uia_worker().start();
      start_watch_thread();
      g_watch_cv.notify_all();
    }
  }

  void session_stopped() {
    g_streaming.store(false);
    // 串流結束後選單沒有人看得到，留在桌面上只會擋住本機的使用者
    if (const HWND hwnd = g_menu_hwnd.load()) {
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
  }

  bool on_mouse_button(int button, bool release) {
    if (button != 1) {
      return false;
    }
    if (release) {
      // 按下被吞了，放開也要吞：只注入放開的話，游標底下的東西會收到沒頭沒尾的一下
      return g_left_swallowed.exchange(false, std::memory_order_relaxed);
    }
    g_left_swallowed.store(false, std::memory_order_relaxed);

    if (!armed() || g_menu_open.load(std::memory_order_relaxed)) {
      return false;
    }

    // §STREAM-MENU：一般點擊的成本到這裡為止——游標底下的頂層視窗屬於哪個行程（pid 有快取）。
    POINT pt;
    if (!GetCursorPos(&pt)) {
      return false;
    }
    const HWND under = WindowFromPoint(pt);
    if (!under) {
      return false;
    }
    const HWND root = GetAncestor(under, GA_ROOT);
    DWORD pid = 0;
    GetWindowThreadProcessId(root ? root : under, &pid);
    if (!pid || !image_name_is(pid, L"StartMenuExperienceHost.exe", g_start_menu_pid)) {
      return false;
    }

    // 以下只有點在開始選單上才會走到
    if (::vr::active()) {
      return false;  // VR session：桌面不是使用者正在看的畫面，不介入
    }
    if (uia_worker().probe(pt, k_uia_timeout) != probe_e::hit) {
      return false;
    }

    g_left_swallowed.store(true, std::memory_order_relaxed);
    send_escape();  // 關掉開始選單
    show_menu(source_e::power_button);
    return true;
  }

  bool on_key(std::uint16_t vk, bool release) {
    unsigned alt_bit = 0;
    switch (vk) {
      case VK_MENU:
        alt_bit = 1;
        break;
      case VK_LMENU:
        alt_bit = 2;
        break;
      case VK_RMENU:
        alt_bit = 4;
        break;
      case VK_F4:
        break;
      default:
        return false;
    }

    if (alt_bit) {
      // 只記遠端送來的 Alt（不問系統的鍵盤狀態）：本機鍵盤按著 Alt 不應該讓遠端的 F4 被攔
      if (release) {
        g_alt_mask.fetch_and(~alt_bit, std::memory_order_relaxed);
      } else {
        g_alt_mask.fetch_or(alt_bit, std::memory_order_relaxed);
      }
      return false;
    }

    // F4
    const std::uint64_t swallowed_at = g_f4_swallowed_ms.load(std::memory_order_relaxed);
    const std::uint64_t now = GetTickCount64();
    const bool swallowing = swallowed_at != 0 && now - swallowed_at < k_f4_swallow_max_ms;
    if (release) {
      g_f4_swallowed_ms.store(0, std::memory_order_relaxed);
      return swallowing;
    }
    if (swallowing) {
      // 按住不放的重複：選單這時多半已經在前景，注入的話 Alt+F4 會落在選單自己身上
      return true;
    }

    if (!g_alt_mask.load(std::memory_order_relaxed) || !armed()) {
      return false;
    }
    if (!foreground_is_desktop() || ::vr::active()) {
      return false;
    }

    g_f4_swallowed_ms.store(now ? now : 1, std::memory_order_relaxed);
    show_menu(source_e::alt_f4);
    return true;
  }
}  // namespace platf::stream_menu
