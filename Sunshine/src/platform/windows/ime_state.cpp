/**
 * @file src/platform/windows/ime_state.cpp
 * @brief §KBD-IME-STATE：用 WM_IME_CONTROL 讀寫前景視窗的輸入法，不注入 Shift。
 *
 * 輸入執行緒是唯一的 task_pool 執行緒（見 input.cpp send_input 的 §INPUT-SENDINPUT）。
 * 這裡對別的行程送訊息，全部用 SendMessageTimeout，整段約 500 ms 就停，避免鍵鼠無限期排隊。
 */
#define WINVER 0x0A00

#include <Windows.h>
#include <imm.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#include "ime_state.h"
#include "misc.h"
#include "src/platform/common.h"

namespace {
  using namespace std::chrono_literals;

  constexpr auto k_budget = 500ms;
  constexpr auto k_per_msg = 200ms;
  constexpr auto k_settle = 30ms;

  static_assert(VIPLE_KBE_FLAG_IME_TOGGLE == 0x80);
  static_assert((VIPLE_KBE_FLAG_IME_TOGGLE & SS_KBE_FLAG_NON_NORMALIZED) == 0);
  // WM_IME_CONTROL 的子命令。MinGW 的 imm.h 沒有這四個（MSVC 的 SDK 有），數值固定，自己定義。
  constexpr WPARAM k_imc_get_conversion_mode = 0x0001;
  constexpr WPARAM k_imc_set_conversion_mode = 0x0002;
  constexpr WPARAM k_imc_get_open_status = 0x0005;
  constexpr WPARAM k_imc_set_open_status = 0x0006;
  static_assert(IME_CMODE_NATIVE == 0x0001);

  std::string hex32(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", static_cast<unsigned>(v));
    return buf;
  }

  // 只取視窗類別。不呼叫 GetWindowText，標題可能是使用者正在打的字。
  std::string class_utf8(HWND hwnd) {
    wchar_t wide[256] = {};
    const int n = GetClassNameW(hwnd, wide, static_cast<int>(sizeof(wide) / sizeof(wide[0])));
    if (n <= 0) {
      return "?";
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, n, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
      return "?";
    }
    std::string raw(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, n, raw.data(), bytes, nullptr, nullptr);
    std::string clean;
    clean.reserve(raw.size());
    for (unsigned char c : raw) {
      if (clean.size() >= 128) {
        break;
      }
      if (c < 0x20 || c == 0x7f) {
        continue;
      }
      clean.push_back(static_cast<char>(c));
    }
    return clean.empty() ? "?" : clean;
  }

  bool is_chinese(uint32_t open, uint32_t conv) {
    return open != 0 && (conv & IME_CMODE_NATIVE) != 0;
  }

  // 每次呼叫結束印一行。提早返回也會印（結果欄說明停在哪）。
  struct trace_t {
    std::string fg = "?";
    std::string open_a = "?";
    std::string open_b = "?";
    std::string conv_a = "?";
    std::string conv_b = "?";
    const char *result = "timeout";

    ~trace_t() {
      BOOST_LOG(info) << "[VIPLE-IME] toggle by state: fg="sv << fg
                      << " open "sv << open_a << "->"sv << open_b
                      << " conv "sv << conv_a << "->"sv << conv_b
                      << " result="sv << result;
    }
  };
}  // namespace

namespace platf {
  bool ime_toggle_by_state() {
    trace_t tr;
    const auto t0 = std::chrono::steady_clock::now();

    auto ms_left = [&]() -> UINT {
      auto left = k_budget - (std::chrono::steady_clock::now() - t0);
      if (left <= 0ms) {
        return 0;
      }
      if (left > k_per_msg) {
        left = k_per_msg;
      }
      return static_cast<UINT>(std::chrono::duration_cast<std::chrono::milliseconds>(left).count());
    };

    auto send = [&](HWND hwnd, WPARAM cmd, LPARAM lp, DWORD_PTR *out) -> bool {
      const UINT ms = ms_left();
      if (ms == 0) {
        tr.result = "timeout";
        return false;
      }
      DWORD_PTR dw = 0;
      const LRESULT r = SendMessageTimeoutW(hwnd, WM_IME_CONTROL, cmd, lp, SMTO_ABORTIFHUNG | SMTO_BLOCK, ms, &dw);
      if (r == 0) {
        tr.result = "timeout";
        return false;
      }
      if (out) {
        *out = dw;
      }
      return true;
    };

    // GetForegroundWindow 只看這個執行緒所在的桌面。不在輸入桌面時會是 NULL；
    // 跟 send_input 失敗時一樣，同步一次再試。
    HWND fg = GetForegroundWindow();
    if (!fg) {
      std::wstring desk_name;
      syncThreadDesktop(&desk_name);
      fg = GetForegroundWindow();
    }
    if (!fg) {
      tr.fg = "(none)";
      tr.result = "no-ime-window";
      return false;
    }
    tr.fg = class_utf8(fg);

    HWND target = fg;
    const DWORD tid = GetWindowThreadProcessId(fg, nullptr);
    if (tid != 0) {
      GUITHREADINFO gi {};
      gi.cbSize = sizeof(gi);
      if (GetGUIThreadInfo(tid, &gi) && gi.hwndFocus) {
        target = gi.hwndFocus;
      }
    }

    const HWND ime = ImmGetDefaultIMEWnd(target);
    if (!ime) {
      tr.result = "no-ime-window";
      return false;
    }

    DWORD_PTR open_dw = 0;
    DWORD_PTR conv_dw = 0;
    if (!send(ime, k_imc_get_open_status, 0, &open_dw) || !send(ime, k_imc_get_conversion_mode, 0, &conv_dw)) {
      return false;
    }
    const uint32_t open0 = static_cast<uint32_t>(open_dw);
    const uint32_t conv0 = static_cast<uint32_t>(conv_dw);
    tr.open_a = std::to_string(open0);
    tr.conv_a = hex32(conv0);

    const bool zh = is_chinese(open0, conv0);
    if (!zh && open0 == 0) {
      if (!send(ime, k_imc_set_open_status, 1, nullptr)) {
        return false;
      }
    }
    const uint32_t next = zh ? (conv0 & ~static_cast<uint32_t>(IME_CMODE_NATIVE))
                             : (conv0 | static_cast<uint32_t>(IME_CMODE_NATIVE));
    if (!send(ime, k_imc_set_conversion_mode, static_cast<LPARAM>(next), nullptr)) {
      return false;
    }

    const auto left = k_budget - (std::chrono::steady_clock::now() - t0);
    if (left < k_settle) {
      tr.result = "timeout";
      return false;
    }
    Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(k_settle).count()));

    DWORD_PTR open1_dw = 0;
    DWORD_PTR conv1_dw = 0;
    if (!send(ime, k_imc_get_open_status, 0, &open1_dw)) {
      return false;
    }
    tr.open_b = std::to_string(static_cast<uint32_t>(open1_dw));
    if (!send(ime, k_imc_get_conversion_mode, 0, &conv1_dw)) {
      return false;
    }
    const uint32_t open1 = static_cast<uint32_t>(open1_dw);
    const uint32_t conv1 = static_cast<uint32_t>(conv1_dw);
    tr.conv_b = hex32(conv1);

    if (is_chinese(open1, conv1) == zh) {
      tr.result = "unchanged";
      return false;
    }
    tr.result = "ok";
    return true;
  }
}  // namespace platf
