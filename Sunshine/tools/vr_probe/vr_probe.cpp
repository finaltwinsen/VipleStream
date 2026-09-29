// vr_probe.cpp - VipleStream §VR：vr_probe 主程式（MSVC，不進 CMake、不出貨；§F.4）。
//
//   vr_probe.exe --mode <whoami|space|timing|scene|watch|ipcpeer|unit> [--out <dir>] [--seconds N] [...]
//
// V2 實作 ipcpeer 與 unit（都不需要 SteamVR）；V4 加入 whoami／space／timing／scene／watch（probe_openvr.cpp）。
// 由 Build-SteamVRDriver.ps1 -Target probe 建置；輸出 out\x64\Release\vr_probe\{vr_probe.exe, openvr_api.dll}。
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>

#include <windows.h>
#include <shlobj.h>

// vendored OpenVR（/external:I，/external:W0）。openvr_api.dll 以 /DELAYLOAD 載入：
// ipcpeer／unit 不會碰到它，在沒有 SteamVR 的機器上也能跑。
#include <openvr.h>

#include "probe_common.h"
#include "vr_ipc_abi_table.h"

namespace probe {
  namespace {
    SRWLOCK g_out_lock = SRWLOCK_INIT;
    int64_t g_qpf = 0;
  }  // namespace

  void line(const char *fmt, ...) {
    char body[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n < 0) {
      return;
    }
    // server 的過濾規則（§F.4）：限長 240、控制字元與 '[' 換成 '?'。這裡先照做，讓行在過濾前後相同。
    constexpr size_t k_prefix = sizeof("[VIPLE-VR-PROBE] ") - 1;
    char out[k_prefix + 240 + 2];
    std::memcpy(out, "[VIPLE-VR-PROBE] ", k_prefix);
    size_t len = 0;
    for (; body[len] != '\0' && len < 239; ++len) {
      const unsigned char ch = (unsigned char) body[len];
      out[k_prefix + len] = (ch < 0x20 || ch >= 0x7F || ch == '[') ? '?' : (char) ch;
    }
    out[k_prefix + len] = '\r';
    out[k_prefix + len + 1] = '\n';
    const DWORD total = (DWORD) (k_prefix + len + 2);
    AcquireSRWLockExclusive(&g_out_lock);
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) {
      DWORD written = 0;
      WriteFile(h, out, total, &written, nullptr);  // 失敗（server 被殺、pipe 斷）一律忽略
    }
    ReleaseSRWLockExclusive(&g_out_lock);
  }

  void print_abi_table() {
    for (const auto &row : vripc_abi_table::k_rows) {
      line("abi-row %s", vripc_abi_table::format_row(row).c_str());
    }
    line("abi-rows count=%u digest=%016llx", (unsigned) vripc_abi_table::k_row_count, (unsigned long long) vripc_abi_table::table_digest());
  }

  int64_t qpc() {
    LARGE_INTEGER v;
    QueryPerformanceCounter(&v);
    return v.QuadPart;
  }

  int64_t qpf() {
    if (g_qpf == 0) {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      g_qpf = f.QuadPart;
    }
    return g_qpf;
  }

  double qpc_to_ms(int64_t ticks) {
    return (double) ticks * 1000.0 / (double) qpf();
  }

  bool prepare_out_dir(const std::wstring &requested, std::wstring &out_dir) {
    std::wstring dir = requested;
    if (dir.empty()) {
      PWSTR base = nullptr;
      // flags = 0：不可 KF_FLAG_CREATE（§S1-13 的同一條規則）
      if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) || !base) {
        if (base) {
          CoTaskMemFree(base);
        }
        return false;
      }
      SYSTEMTIME st;
      GetSystemTime(&st);
      wchar_t stamp[64];
      swprintf(stamp, 64, L"%04u%02u%02uT%02u%02u%02uZ-%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
      dir = std::wstring(base) + L"\\VipleStream\\vr_probe\\" + stamp;
      CoTaskMemFree(base);
    }
    const int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS && rc != ERROR_FILE_EXISTS) {
      return false;
    }
    out_dir = dir;
    return true;
  }

  csv_t::~csv_t() {
    close();
  }

  bool csv_t::open(const std::wstring &dir, const wchar_t *name, const char *header) {
    close();
    if (dir.empty()) {
      return false;
    }
    const std::wstring path = dir + L"\\" + name;
    if (_wfopen_s(&f_, path.c_str(), L"wb") != 0) {
      f_ = nullptr;
      return false;
    }
    std::fprintf(f_, "%s\n", header);
    return true;
  }

  void csv_t::row(const char *fmt, ...) {
    if (!f_) {
      return;
    }
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(f_, fmt, ap);
    va_end(ap);
    std::fputc('\n', f_);
  }

  void csv_t::close() {
    if (f_) {
      std::fflush(f_);
      std::fclose(f_);
      f_ = nullptr;
    }
  }
}  // namespace probe

namespace {
  std::string narrow(const wchar_t *w) {
    std::string s;
    for (; *w; ++w) {
      s.push_back((*w >= 0x20 && *w < 0x7F) ? (char) *w : '?');
    }
    return s;
  }

  bool parse_u32(const wchar_t *s, uint32_t &out) {
    wchar_t *end = nullptr;
    const unsigned long long v = std::wcstoull(s, &end, 10);
    if (!end || *end != L'\0' || end == s || v > 0xFFFFFFFFull) {
      return false;
    }
    out = (uint32_t) v;
    return true;
  }

  bool parse_double(const wchar_t *s, double &out) {
    wchar_t *end = nullptr;
    const double v = std::wcstod(s, &end);
    if (!end || *end != L'\0' || end == s || !(v >= 0.0) || v > 86400.0) {
      return false;
    }
    out = v;
    return true;
  }

  void usage() {
    probe::line("usage vr_probe --mode whoami|space|timing|scene|watch|ipcpeer|unit --out DIR --seconds N");
    probe::line("usage ipcpeer: --abi N --hello-size B --peer-stop-signal-after N --peer-crash-after N --peer-never-read");
    probe::line("usage ipcpeer: --peer-close-after-welcome --peer-fence-max --peer-fence-max-after N --peer-escalate");
    probe::line("usage ipcpeer: --peer-gpu-hold-ms X --peer-writers N --peer-no-flush --peer-no-render --hello-repeat N --hello-interval-ms M --pipe NAME");
    probe::line("usage unit: --no-loopback");
    probe::line("usage space: --reset-seated");
  }
}  // namespace

int wmain(int argc, wchar_t **argv) {
  probe::args_t a;
  bool ok = true;
  for (int i = 1; i < argc && ok; ++i) {
    const std::wstring k = argv[i];
    const bool has_v = i + 1 < argc;
    auto need = [&](const wchar_t *&v) -> bool {
      if (!has_v) {
        return false;
      }
      v = argv[++i];
      return true;
    };
    const wchar_t *v = nullptr;
    if (k == L"--mode") {
      ok = need(v);
      if (ok) {
        a.mode = narrow(v);
      }
    } else if (k == L"--out") {
      ok = need(v);
      if (ok) {
        a.out = v;
      }
    } else if (k == L"--seconds") {
      ok = need(v) && parse_double(v, a.seconds);
    } else if (k == L"--pipe") {
      ok = need(v);
      if (ok) {
        a.pipe = v;
      }
    } else if (k == L"--abi") {
      ok = need(v) && parse_u32(v, a.abi);
    } else if (k == L"--hello-size") {
      ok = need(v) && parse_u32(v, a.hello_size) && a.hello_size >= 24 && a.hello_size <= 512;
    } else if (k == L"--peer-stop-signal-after") {
      ok = need(v) && parse_u32(v, a.stop_signal_after);
    } else if (k == L"--peer-crash-after") {
      ok = need(v) && parse_u32(v, a.crash_after);
    } else if (k == L"--peer-never-read") {
      a.never_read = true;
    } else if (k == L"--peer-close-after-welcome") {
      a.close_after_welcome = true;
    } else if (k == L"--peer-fence-max") {
      a.fence_max = true;
    } else if (k == L"--peer-fence-max-after") {
      ok = need(v) && parse_u32(v, a.fence_max_after);
    } else if (k == L"--peer-escalate") {
      a.escalate = true;
    } else if (k == L"--peer-gpu-hold-ms") {
      ok = need(v) && parse_u32(v, a.gpu_hold_ms) && a.gpu_hold_ms <= 1000;
    } else if (k == L"--peer-writers") {
      ok = need(v) && parse_u32(v, a.writers);
    } else if (k == L"--peer-no-flush") {
      a.no_flush = true;
    } else if (k == L"--peer-no-render") {
      a.no_render = true;
    } else if (k == L"--hello-repeat") {
      ok = need(v) && parse_u32(v, a.hello_repeat) && a.hello_repeat <= 100;
    } else if (k == L"--hello-interval-ms") {
      ok = need(v) && parse_u32(v, a.hello_interval_ms) && a.hello_interval_ms >= 10 && a.hello_interval_ms <= 60000;
    } else if (k == L"--no-loopback") {
      a.no_loopback = true;
    } else if (k == L"--reset-seated") {
      a.reset_seated = true;
    } else {
      probe::line("error reason=unknown-arg arg=%s", narrow(k.c_str()).c_str());
      ok = false;
    }
  }
  if (!ok || a.mode.empty()) {
    if (ok) {
      probe::line("error reason=missing-mode");
    } else {
      probe::line("error reason=bad-args");
    }
    usage();
    return probe::rc_usage;
  }

  probe::line("start mode=%s pid=%lu openvr_sdk=%u.%u.%u abi=%u", a.mode.c_str(), GetCurrentProcessId(), vr::k_nSteamVRVersionMajor, vr::k_nSteamVRVersionMinor, vr::k_nSteamVRVersionBuild, (unsigned) VRIPC_ABI_VERSION);

  if (a.mode == "ipcpeer") {
    return probe::run_ipcpeer(a);
  }
  if (a.mode == "unit") {
    return probe::run_unit(a);
  }
  if (a.mode == "whoami") {
    return probe::run_whoami(a);
  }
  if (a.mode == "scene") {
    return probe::run_scene(a);
  }
  if (a.mode == "timing") {
    return probe::run_timing(a);
  }
  if (a.mode == "watch") {
    return probe::run_watch(a);
  }
  if (a.mode == "space") {
    return probe::run_space(a);
  }
  probe::line("error reason=unknown-mode mode=%s", a.mode.c_str());
  usage();
  return probe::rc_usage;
}
