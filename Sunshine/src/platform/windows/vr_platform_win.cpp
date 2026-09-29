/**
 * @file src/platform/windows/vr_platform_win.cpp
 * @brief VipleStream 2.0 §VR：Windows 平台的 VR 支援（M1b V5：SteamVR 探索、部署、註冊、settings guard、衝突偵測）。
 *
 * 介面與執行緒規則見 src/vr/vr_platform.h。重點：
 *   - 讀使用者可寫檔案一律 `FILE_FLAG_OPEN_REPARSE_POINT`、拒絕 reparse point、以已開 handle 的最終路徑比對預期路徑、
 *     有大小上限；有主控台使用者時在模擬身分下讀（sec-m5、sec-m6）。
 *   - 寫 steamvr.vrsettings 只在模擬身分下（不變式 9）；vrpathreg 以使用者 limited token 執行。
 *   - 部署目的地 `<install>\config\steamvr\<ver>\viplestream` 用 protected DACL（SY/BA 完全、BU 讀取＋執行）。
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <vector>

// lib includes（boost 要在 Windows.h 之前，理由同 misc.cpp）
#include <boost/filesystem/path.hpp>
#include <boost/process/v1.hpp>
#include <nlohmann/json.hpp>

// platform includes
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <ShlObj.h>
#include <tlhelp32.h>

// local includes
#include "misc.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/self_update.h"
#include "src/vr/vr_bridge.h"
#include "src/vr/vr_ipc_abi.h"
#include "src/vr/vr_platform.h"
#include "steam_scanner.h"
#include "utf_utils.h"
#include "vdf_parser.h"

using namespace std::literals;

namespace platf::dxgi {
  bool vr_pick_adapter_luid(LUID &out, std::string &why);  // display_vr.cpp（K22）
}

namespace vr {
  bool platform_supported() {
    return true;
  }

  namespace platform {
    namespace {
      namespace fs = std::filesystem;
      using steady = std::chrono::steady_clock;

      constexpr size_t k_cap_1m = 1u << 20;
      constexpr size_t k_cap_4m = 4u << 20;
      constexpr const wchar_t *k_protected_sddl = L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
      constexpr const char *k_ti_sid = "S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

      struct handle_t {
        HANDLE h = nullptr;

        handle_t() = default;

        explicit handle_t(HANDLE v):
            h(v) {
        }

        handle_t(const handle_t &) = delete;
        handle_t &operator=(const handle_t &) = delete;

        handle_t(handle_t &&o) noexcept:
            h(std::exchange(o.h, nullptr)) {
        }

        handle_t &operator=(handle_t &&o) noexcept {
          if (this != &o) {
            reset();
            h = std::exchange(o.h, nullptr);
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

      // ── 共用狀態（快取）─────────────────────────────────────────────────
      std::mutex g_mtx;
      env_t g_env;
      steady::time_point g_env_at {};
      std::atomic<bool> g_disabled {false};
      conflict_t g_conf;
      std::set<std::string> g_vr_ids;
      bool g_vr_ids_loaded = false;

      fs::path install_dir() {
        std::wstring buf(MAX_PATH, L'\0');
        for (;;) {
          const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
          if (n == 0) {
            return {};
          }
          if (n < buf.size()) {
            buf.resize(n);
            break;
          }
          buf.resize(buf.size() * 2);
        }
        return fs::path {buf}.parent_path();
      }

      fs::path state_root() {
        return install_dir() / L"config" / L"steamvr";
      }

      std::wstring lower(std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
          return static_cast<wchar_t>(std::towlower(c));
        });
        return s;
      }

      /// 比對用的正規化：斜線、`..`、大小寫、尾端反斜線
      std::wstring norm(const fs::path &p) {
        auto s = lower(fs::path {p}.make_preferred().lexically_normal().wstring());
        while (s.size() > 3 && s.back() == L'\\') {
          s.pop_back();
        }
        if (s.size() == 3 && s[1] == L':' && s[2] == L'\\') {
          s.pop_back();
        }
        return s;
      }

      bool same_path(const fs::path &a, const fs::path &b) {
        return norm(a) == norm(b);
      }

      /// 路徑只印尾端（前面可能含使用者名稱）
      std::string tail(const fs::path &p) {
        const auto s = utf_utils::to_utf8(p.wstring());
        const auto pos = s.find_last_of("\\/");
        return pos == std::string::npos ? s : ".." + s.substr(pos);
      }

      /// 已開 handle 的最終路徑（去掉 `\\?\`）；網路路徑回空
      std::wstring final_path(HANDLE h) {
        std::wstring buf(1024, L'\0');
        DWORD n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (n >= buf.size()) {
          buf.resize(n + 1);
          n = GetFinalPathNameByHandleW(h, buf.data(), static_cast<DWORD>(buf.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        }
        if (n == 0 || n >= buf.size()) {
          return {};
        }
        buf.resize(n);
        if (buf.starts_with(L"\\\\?\\UNC\\")) {
          return {};
        }
        if (buf.starts_with(L"\\\\?\\")) {
          buf.erase(0, 4);
        }
        return buf;
      }

      bool is_reparse(HANDLE h) {
        FILE_ATTRIBUTE_TAG_INFO tag {};
        if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag, sizeof(tag))) {
          return true;  // 查不到就當成不安全
        }
        return (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
      }

      // ── 主控台使用者 ─────────────────────────────────────────────────────
      struct user_t {
        handle_t token;
        std::wstring sid;
        DWORD rid = 0;
      };

      std::optional<user_t> console_user() {
        handle_t tok {platf::retrieve_users_token(false)};
        if (!tok) {
          return std::nullopt;
        }
        user_t u;
        DWORD len = 0;
        GetTokenInformation(tok.h, TokenUser, nullptr, 0, &len);
        std::vector<BYTE> buf(len ? len : 1);
        if (len && GetTokenInformation(tok.h, TokenUser, buf.data(), len, &len)) {
          PSID sid = reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid;
          LPWSTR str = nullptr;
          if (ConvertSidToStringSidW(sid, &str)) {
            u.sid = str;
            LocalFree(str);
          }
          const UCHAR n = *GetSidSubAuthorityCount(sid);
          u.rid = n ? *GetSidSubAuthority(sid, n - 1) : 0;
        }
        u.token = std::move(tok);
        if (u.sid.empty()) {
          return std::nullopt;
        }
        return u;
      }

      /// 以使用者身分執行（有使用者時）；沒有使用者時 allow_system 決定要不要以 SYSTEM 執行
      bool as_user(const std::function<void()> &fn, bool allow_system) {
        auto u = console_user();
        if (!u) {
          if (allow_system) {
            fn();
            return true;
          }
          return false;
        }
        return !platf::impersonate_current_user(u->token.h, fn);
      }

      // ── 安全讀寫使用者可寫檔案 ─────────────────────────────────────────
      enum class rd_e {
        ok,
        missing,
        reparse,
        path,
        too_big,
        io,
      };

      const char *rd_name(rd_e r) {
        switch (r) {
          case rd_e::ok:
            return "ok";
          case rd_e::missing:
            return "missing";
          case rd_e::reparse:
            return "reparse";
          case rd_e::path:
            return "path";
          case rd_e::too_big:
            return "too-big";
          default:
            return "io";
        }
      }

      /// 呼叫端決定身分；tail=true 時超過上限只讀尾端 cap 位元組
      rd_e read_file_checked(const fs::path &p, size_t cap, bool tail_mode, std::string &out) {
        out.clear();
        handle_t h {CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr)};
        if (!h) {
          const DWORD e = GetLastError();
          return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? rd_e::missing : rd_e::io;
        }
        if (is_reparse(h.h)) {
          return rd_e::reparse;
        }
        if (!same_path(final_path(h.h), p)) {
          return rd_e::path;
        }
        LARGE_INTEGER size {};
        if (!GetFileSizeEx(h.h, &size) || size.QuadPart < 0) {
          return rd_e::io;
        }
        uint64_t want = static_cast<uint64_t>(size.QuadPart);
        if (want > cap) {
          if (!tail_mode) {
            return rd_e::too_big;
          }
          LARGE_INTEGER off;
          off.QuadPart = size.QuadPart - static_cast<LONGLONG>(cap);
          if (!SetFilePointerEx(h.h, off, nullptr, FILE_BEGIN)) {
            return rd_e::io;
          }
          want = cap;
        }
        out.resize(static_cast<size_t>(want));
        size_t got = 0;
        while (got < out.size()) {
          DWORD n = 0;
          if (!ReadFile(h.h, out.data() + got, static_cast<DWORD>(std::min<size_t>(out.size() - got, 1u << 20)), &n, nullptr)) {
            return rd_e::io;
          }
          if (n == 0) {
            break;
          }
          got += n;
        }
        out.resize(got);
        return rd_e::ok;
      }

      /// 有使用者時在模擬身分下讀，沒有時以 SYSTEM 讀
      rd_e read_user_file(const fs::path &p, size_t cap, bool tail_mode, std::string &out) {
        rd_e r = rd_e::io;
        if (!as_user([&]() {
              r = read_file_checked(p, cap, tail_mode, out);
            },
                     true)) {
          return rd_e::io;
        }
        return r;
      }

      /// 在目前身分下以暫存檔＋ReplaceFileW 原子寫入（呼叫端負責模擬）
      bool write_file_atomic(const fs::path &target, const std::string &data, std::string &why) {
        const fs::path tmp = fs::path {target}.concat(L".viple-tmp");
        {
          handle_t h {CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
          if (!h) {
            why = std::format("create tmp err={}", GetLastError());
            return false;
          }
          if (is_reparse(h.h) || !same_path(final_path(h.h), tmp)) {
            why = "tmp path";
            return false;
          }
          DWORD n = 0;
          if (!WriteFile(h.h, data.data(), static_cast<DWORD>(data.size()), &n, nullptr) || n != data.size() || !FlushFileBuffers(h.h)) {
            why = std::format("write err={}", GetLastError());
            return false;
          }
        }
        const DWORD attrs = GetFileAttributesW(target.c_str());
        const bool ok = attrs == INVALID_FILE_ATTRIBUTES ? MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) :
                                                           ReplaceFileW(target.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
        if (!ok) {
          why = std::format("replace err={}", GetLastError());
          DeleteFileW(tmp.c_str());
          return false;
        }
        return true;
      }

      /// SYSTEM 寫 protected 目錄內的檔案（marker、state）
      bool write_system_file(const fs::path &target, const std::string &data) {
        std::string why;
        return write_file_atomic(target, data, why);
      }

      // ── registry ─────────────────────────────────────────────────────────
      std::wstring reg_sz(HKEY root, const wchar_t *key, const wchar_t *value) {
        DWORD bytes = 0;
        if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) {
          return {};
        }
        std::wstring s(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, s.data(), &bytes) != ERROR_SUCCESS) {
          return {};
        }
        s.resize(wcsnlen(s.c_str(), s.size()));
        return s;
      }

      std::optional<DWORD> reg_dword(HKEY root, const std::wstring &key, const wchar_t *value) {
        DWORD v = 0, bytes = sizeof(v);
        if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_DWORD, nullptr, &v, &bytes) != ERROR_SUCCESS) {
          return std::nullopt;
        }
        return v;
      }

      // ── 行程 ─────────────────────────────────────────────────────────────
      struct proc_t {
        DWORD pid = 0;
        std::wstring image;
      };

      /// 主控台 session 內映像名稱為 exe 的行程（附完整映像路徑）
      std::vector<proc_t> console_procs(const wchar_t *exe) {
        std::vector<proc_t> out;
        const DWORD console = WTSGetActiveConsoleSessionId();
        handle_t snap {CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
        if (!snap) {
          return out;
        }
        PROCESSENTRY32W pe {};
        pe.dwSize = sizeof(pe);
        for (BOOL ok = Process32FirstW(snap.h, &pe); ok; ok = Process32NextW(snap.h, &pe)) {
          if (_wcsicmp(pe.szExeFile, exe) != 0) {
            continue;
          }
          DWORD sess = 0;
          if (!ProcessIdToSessionId(pe.th32ProcessID, &sess) || sess != console) {
            continue;
          }
          proc_t p;
          p.pid = pe.th32ProcessID;
          handle_t h {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)};
          if (h) {
            std::wstring buf(1024, L'\0');
            DWORD n = static_cast<DWORD>(buf.size());
            if (QueryFullProcessImageNameW(h.h, 0, buf.data(), &n)) {
              buf.resize(n);
              p.image = buf;
            }
          }
          out.push_back(std::move(p));
        }
        return out;
      }

      bool wait_pid_exit(DWORD pid, std::chrono::milliseconds timeout) {
        handle_t h {OpenProcess(SYNCHRONIZE, FALSE, pid)};
        if (!h) {
          return true;  // 已經不在
        }
        return WaitForSingleObject(h.h, static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
      }

      int terminate_all(const wchar_t *exe) {
        int n = 0;
        for (const auto &p : console_procs(exe)) {
          handle_t h {OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, p.pid)};
          if (h && TerminateProcess(h.h, 1)) {
            WaitForSingleObject(h.h, 3000);
            ++n;
          }
        }
        return n;
      }

      // ── openvrpaths ──────────────────────────────────────────────────────
      struct openvrpaths_t {
        std::vector<std::wstring> runtime;
        std::vector<std::wstring> config;
        std::vector<std::wstring> external_drivers;
      };

      std::optional<fs::path> user_local_appdata() {
        auto u = console_user();
        if (!u) {
          return std::nullopt;
        }
        PWSTR base = nullptr;
        // flags=0：不可 KF_FLAG_CREATE（§S1-13）
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, u->token.h, &base)) || !base) {
          if (base) {
            CoTaskMemFree(base);
          }
          return std::nullopt;
        }
        fs::path p {base};
        CoTaskMemFree(base);
        return p;
      }

      std::optional<openvrpaths_t> read_openvrpaths() {
        const auto base = user_local_appdata();
        if (!base) {
          return std::nullopt;
        }
        std::string text;
        if (read_user_file(*base / L"openvr" / L"openvrpaths.vrpath", k_cap_1m, false, text) != rd_e::ok) {
          return std::nullopt;
        }
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
          return std::nullopt;
        }
        openvrpaths_t o;
        auto grab = [&](const char *key, std::vector<std::wstring> &dst) {
          if (j.contains(key) && j[key].is_array()) {
            for (const auto &v : j[key]) {
              if (v.is_string()) {
                dst.push_back(utf_utils::from_utf8(v.get<std::string>()));
              }
            }
          }
        };
        grab("runtime", o.runtime);
        grab("config", o.config);
        grab("external_drivers", o.external_drivers);
        return o;
      }

      // ── 安裝目錄的 owner／DACL 檢查（§D.11 第 1 點）──────────────────
      bool sid_is(PSID sid, WELL_KNOWN_SID_TYPE t) {
        return IsWellKnownSid(sid, t) != FALSE;
      }

      bool sid_is_ti(PSID sid) {
        LPSTR s = nullptr;
        if (!ConvertSidToStringSidA(sid, &s)) {
          return false;
        }
        const bool ti = std::string_view {s} == k_ti_sid;
        LocalFree(s);
        return ti;
      }

      bool sid_admin(PSID sid) {
        return sid_is(sid, WinLocalSystemSid) || sid_is(sid, WinBuiltinAdministratorsSid) || sid_is_ti(sid);
      }

      /// @return nullopt = 通過；否則 reason（reparse｜path｜owner｜dacl｜volume｜missing）
      std::optional<std::string> check_dir(const fs::path &p, bool ancestor) {
        handle_t h {CreateFileW(p.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!h) {
          return "missing"s;
        }
        if (is_reparse(h.h)) {
          return "reparse"s;
        }
        const auto fp = final_path(h.h);
        if (fp.empty()) {
          return "volume"s;
        }
        if (!same_path(fp, p)) {
          return "path"s;
        }
        DWORD vflags = 0;
        if (!GetVolumeInformationByHandleW(h.h, nullptr, 0, nullptr, nullptr, &vflags, nullptr, 0) || !(vflags & FILE_PERSISTENT_ACLS)) {
          return "volume"s;
        }
        PSID owner = nullptr;
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (GetSecurityInfo(h.h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
          return "dacl"s;
        }
        std::optional<std::string> bad;
        if (!owner || !sid_admin(owner)) {
          bad = "owner"s;
        } else if (!dacl) {
          bad = "dacl"s;
        } else {
          const DWORD install_bad = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
          const DWORD ancestor_bad = DELETE | FILE_DELETE_CHILD | WRITE_DAC | WRITE_OWNER | GENERIC_ALL;
          for (DWORD i = 0; i < dacl->AceCount && !bad; ++i) {
            ACE_HEADER *ace = nullptr;
            if (!GetAce(dacl, i, reinterpret_cast<void **>(&ace)) || ace->AceType != ACCESS_ALLOWED_ACE_TYPE) {
              continue;
            }
            auto *a = reinterpret_cast<ACCESS_ALLOWED_ACE *>(ace);
            PSID sid = &a->SidStart;
            if (sid_admin(sid)) {
              continue;
            }
            const bool inherit_only = (ace->AceFlags & INHERIT_ONLY_ACE) != 0;
            if (inherit_only && (ancestor || sid_is(sid, WinCreatorOwnerSid))) {
              continue;
            }
            if (a->Mask & (ancestor ? ancestor_bad : install_bad)) {
              bad = "dacl"s;
            }
          }
        }
        LocalFree(sd);
        return bad;
      }

      /// 從根到 leaf 逐層檢查：安裝目錄（含）以下用嚴格規則，之上用祖先規則
      std::optional<std::pair<std::string, std::string>> check_chain(const fs::path &leaf) {
        const auto inst = norm(install_dir());
        std::vector<fs::path> chain;
        for (fs::path p = leaf; !p.empty(); p = p.parent_path()) {
          chain.push_back(p);
          if (p == p.parent_path()) {
            break;
          }
        }
        std::reverse(chain.begin(), chain.end());
        for (const auto &p : chain) {
          const auto n = norm(p);
          const bool ancestor = n.size() < inst.size() || (n != inst && !n.starts_with(inst + L"\\"));
          if (auto r = check_dir(p, ancestor)) {
            return std::make_pair(*r, tail(p));
          }
        }
        return std::nullopt;
      }

      /// 建 protected 目錄；已存在時驗證 owner 與非 reparse，並重設成 protected DACL（偏差：設計是改名成 .stale 重建）
      std::optional<std::string> ensure_protected_dir(const fs::path &p) {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(k_protected_sddl, SDDL_REVISION_1, &sd, nullptr)) {
          return "dacl"s;
        }
        SECURITY_ATTRIBUTES sa {sizeof(sa), sd, FALSE};
        std::optional<std::string> r;
        if (!CreateDirectoryW(p.c_str(), &sa)) {
          if (GetLastError() != ERROR_ALREADY_EXISTS) {
            r = "io"s;
          } else {
            handle_t h {CreateFileW(p.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            PSID owner = nullptr;
            PSECURITY_DESCRIPTOR cur = nullptr;
            if (!h) {
              r = "stale-in-use"s;
            } else if (is_reparse(h.h)) {
              r = "reparse"s;
            } else if (!same_path(final_path(h.h), p)) {
              r = "path"s;
            } else if (GetSecurityInfo(h.h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &cur) != ERROR_SUCCESS || !owner || !sid_admin(owner)) {
              r = "owner"s;
            } else {
              BOOL present = FALSE, defaulted = FALSE;
              PACL dacl = nullptr;
              PSID sy = nullptr;
              GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
              GetSecurityDescriptorOwner(sd, &sy, &defaulted);
              if (SetSecurityInfo(h.h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sy, nullptr, dacl, nullptr) != ERROR_SUCCESS) {
                r = "dacl"s;
              }
            }
            if (cur) {
              LocalFree(cur);
            }
          }
        }
        LocalFree(sd);
        return r;
      }

      // ── Steam／SteamVR 探索 ──────────────────────────────────────────────
      struct steamvr_found_t {
        fs::path steam_root;
        fs::path runtime;
        std::string buildid;
        bool installed = false;
      };

      steamvr_found_t find_steamvr() {
        steamvr_found_t f;
        // 不看 SYSTEM 的 HKCU（§S1-13）
        auto root = reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
        if (root.empty()) {
          root = reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
        }
        if (root.empty()) {
          return f;
        }
        f.steam_root = root;
        std::vector<fs::path> libs {f.steam_root};
        std::string text;
        if (read_user_file(f.steam_root / L"steamapps" / L"libraryfolders.vdf", k_cap_1m, false, text) == rd_e::ok) {
          if (auto doc = viple::vdf::parse(text)) {
            if (const auto *lf = doc->child("libraryfolders"); lf && lf->is_map()) {
              for (const auto &[k, v] : lf->as_map()) {
                if (v.is_map()) {
                  const auto path = v.leaf_or("path");
                  if (!path.empty()) {
                    const fs::path lp {utf_utils::from_utf8(path)};
                    if (std::none_of(libs.begin(), libs.end(), [&](const fs::path &x) {
                          return same_path(x, lp);
                        })) {
                      libs.push_back(lp);
                    }
                  }
                }
              }
            }
          }
        }
        for (const auto &lib : libs) {
          if (read_user_file(lib / L"steamapps" / L"appmanifest_250820.acf", k_cap_1m, false, text) != rd_e::ok) {
            continue;
          }
          auto doc = viple::vdf::parse(text);
          const auto *st = doc ? doc->child("AppState") : nullptr;
          if (!st) {
            continue;
          }
          const auto installdir = st->leaf_or("installdir");
          const auto flags = std::strtoul(st->leaf_or("StateFlags", "0").c_str(), nullptr, 10);
          if (installdir.empty() || installdir.find_first_of("\\/:") != std::string::npos || installdir.find("..") != std::string::npos) {
            continue;
          }
          f.runtime = lib / L"steamapps" / L"common" / utf_utils::from_utf8(installdir);
          f.buildid = st->leaf_or("buildid");
          std::error_code ec;
          f.installed = (flags & 4u) && fs::is_regular_file(f.runtime / L"bin" / L"win64" / L"vrserver.exe", ec);
          break;
        }
        // openvrpaths runtime[0] 交叉比對：不一致時以 openvrpaths 為準（兩個都記 log）
        if (auto o = read_openvrpaths(); o && !o->runtime.empty()) {
          const fs::path rt {o->runtime.front()};
          std::error_code ec;
          if (fs::is_regular_file(rt / L"bin" / L"win64" / L"vrserver.exe", ec) && !same_path(rt, f.runtime)) {
            BOOST_LOG(info) << "[VIPLE-VR-ORCH] steamvr runtime differs: appmanifest=" << tail(f.runtime) << " openvrpaths=" << tail(rt) << " (using openvrpaths)";
            f.runtime = rt;
            f.installed = true;
          }
        }
        return f;
      }

      std::wstring vrserver_image_running() {
        for (const auto &p : console_procs(L"vrserver.exe")) {
          if (!p.image.empty()) {
            return p.image;
          }
        }
        return {};
      }

      std::string read_version_file() {
        std::ifstream in(install_dir() / L"steamvr" / L"viplestream.version", std::ios::binary);
        std::string v;
        std::getline(in, v);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) {
          v.pop_back();
        }
        return v;
      }

      // ── vrsettings 讀寫（guard）─────────────────────────────────────────
      struct guard_key_t {
        const char *section;
        const char *key;
        nlohmann::json value;
      };

      std::vector<guard_key_t> guard_keys() {
        return {
          {"steamvr", "forcedDriver", "viplestream"},
          {"driver_vrlink", "enable", false},
        };
      }

      std::optional<fs::path> vrsettings_path() {
        if (auto o = read_openvrpaths(); o && !o->config.empty()) {
          return fs::path {o->config.front()} / L"steamvr.vrsettings";
        }
        const auto env = cached_environment();
        if (!env.steam_root.empty()) {
          return fs::path {utf_utils::from_utf8(env.steam_root)} / L"config" / L"steamvr.vrsettings";
        }
        return std::nullopt;
      }

      fs::path marker_path(const std::wstring &sid) {
        return state_root() / L"guard" / (sid + L".json");
      }

      std::optional<nlohmann::json> read_marker(const fs::path &p) {
        std::string text;
        if (read_file_checked(p, k_cap_1m, false, text) != rd_e::ok) {
          return std::nullopt;
        }
        auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
          return std::nullopt;
        }
        return j;
      }

      std::string utc_now() {
        SYSTEMTIME st {};
        GetSystemTime(&st);
        return std::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
      }

      /// vrsettings 的使用者操作：讀（模擬身分）→ 解析（模擬外）
      enum class vs_e {
        ok,
        missing,
        read_fail,
        parse_fail,
      };

      vs_e load_vrsettings(const fs::path &p, nlohmann::ordered_json &out, std::string &raw) {
        const auto r = read_user_file(p, k_cap_1m, false, raw);
        if (r == rd_e::missing) {
          out = nlohmann::ordered_json::object();
          return vs_e::missing;
        }
        if (r != rd_e::ok) {
          return vs_e::read_fail;
        }
        out = nlohmann::ordered_json::parse(raw, nullptr, false);
        if (out.is_discarded() || !out.is_object()) {
          return vs_e::parse_fail;
        }
        return vs_e::ok;
      }

      bool store_vrsettings(const fs::path &p, const nlohmann::ordered_json &j, const std::string *backup, std::string &why) {
        const std::string data = j.dump(3);
        bool ok = false;
        if (!as_user([&]() {
              // 目錄 handle：不是 reparse、最終路徑與 openvrpaths 給的相同（擋祖先層 junction）
              handle_t d {CreateFileW(p.parent_path().c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
              if (!d || is_reparse(d.h) || !same_path(final_path(d.h), p.parent_path())) {
                why = "config dir path";
                return;
              }
              if (backup) {
                std::string bwhy;
                write_file_atomic(fs::path {p}.concat(L".viplestream-bak"), *backup, bwhy);
              }
              ok = write_file_atomic(p, data, why);
            },
                     false)) {
          why = "no user";
          return false;
        }
        return ok;
      }

      /// 讀一個鍵（沒有回 nullopt）
      std::optional<nlohmann::ordered_json> get_key(const nlohmann::ordered_json &j, const char *section, const char *key) {
        if (!j.contains(section) || !j[section].is_object() || !j[section].contains(key)) {
          return std::nullopt;
        }
        return j[section][key];
      }
    }  // namespace

    // ── 環境 ─────────────────────────────────────────────────────────────
    env_t probe_environment(bool force_refresh) {
      {
        std::lock_guard lk(g_mtx);
        if (!force_refresh && g_env.probed && steady::now() - g_env_at < 60s) {
          return g_env;
        }
      }
      env_t e;
      e.probed = true;
      const auto sv = find_steamvr();
      e.steamvr_installed = sv.installed;
      e.steam_root = utf_utils::to_utf8(sv.steam_root.wstring());
      e.steamvr_runtime = utf_utils::to_utf8(sv.runtime.wstring());
      e.buildid = sv.buildid;

      const auto src = install_dir() / L"steamvr" / L"viplestream";
      if (auto bad = check_chain(src)) {
        e.install_dir_safe = false;
        e.unsafe_reason = bad->first + " level=" + bad->second;
      } else {
        e.install_dir_safe = true;
      }

      e.user_present = console_user().has_value();
      DWORD self_sess = 0;
      ProcessIdToSessionId(GetCurrentProcessId(), &self_sess);
      e.session_ok = platf::is_running_as_system() && self_sess == WTSGetActiveConsoleSessionId();

      LUID luid {};
      std::string why;
      e.adapter_rule_ok = platf::dxgi::vr_pick_adapter_luid(luid, why);
      e.adapter_reason = e.adapter_rule_ok ? std::format("luid=0x{:08x}{:08x}", (uint32_t) luid.HighPart, luid.LowPart) : why;
      e.adapter_luid_low = luid.LowPart;
      e.adapter_luid_high = luid.HighPart;

      const auto xr = reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
      e.openxr_other = !xr.empty() && (sv.runtime.empty() || !norm(xr).starts_with(norm(sv.runtime) + L"\\"));
      e.driver_version = read_version_file();

      bool changed;
      {
        std::lock_guard lk(g_mtx);
        changed = !g_env.probed || g_env.steamvr_installed != e.steamvr_installed || g_env.install_dir_safe != e.install_dir_safe || g_env.adapter_rule_ok != e.adapter_rule_ok ||
                  g_env.session_ok != e.session_ok || g_env.user_present != e.user_present || g_env.buildid != e.buildid;
        g_env = e;
        g_env_at = steady::now();
      }
      if (changed) {
        BOOST_LOG(info) << "[VIPLE-VR-ORCH] env installed=" << e.steamvr_installed << " buildid=" << e.buildid << " runtime=" << tail(sv.runtime) << " installDirSafe=" << e.install_dir_safe
                        << (e.install_dir_safe ? "" : " unsafe=" + e.unsafe_reason) << " user=" << e.user_present << " sessionOk=" << e.session_ok << " adapter=" << e.adapter_rule_ok << " (" << e.adapter_reason
                        << ") openxrOther=" << e.openxr_other << " driverVer=" << e.driver_version;
      }
      return e;
    }

    env_t cached_environment() {
      std::lock_guard lk(g_mtx);
      return g_env;
    }

    bool pcvr_available(std::string *reason) {
      auto set = [&](const char *r) {
        if (reason) {
          *reason = r;
        }
        return false;
      };
      if (g_disabled.load()) {
        return set("disabled until restart");
      }
      const auto e = cached_environment();
      if (!e.probed) {
        return set("not probed yet");
      }
      if (!e.steamvr_installed) {
        return set("steamvr not installed");
      }
      if (!e.install_dir_safe) {
        return set("install dir unsafe");
      }
      if (!e.session_ok) {
        return set("session mismatch");
      }
      if (!e.user_present) {
        return set("no console user");
      }
      if (!e.adapter_rule_ok) {
        return set("adapter mismatch");
      }
      const auto ph = vr::bridge::status().phase;
      if (ph == vr::bridge::phase_e::stopped || ph == vr::bridge::phase_e::no_pipe) {
        return set("pipe unavailable");
      }
      return true;
    }

    void disable_pcvr_until_restart() {
      g_disabled.store(true);
    }

    bool console_user_present() {
      handle_t tok {platf::retrieve_users_token(false)};
      return static_cast<bool>(tok);
    }

    uint32_t console_user_rid() {
      auto u = console_user();
      return u ? u->rid : 0;
    }

    uint32_t vrserver_pid() {
      const auto v = console_procs(L"vrserver.exe");
      return v.empty() ? 0 : v.front().pid;
    }

    std::wstring expected_driver_host_image() {
      auto img = vrserver_image_running();
      if (img.empty()) {
        const auto e = cached_environment();
        if (!e.steamvr_runtime.empty()) {
          img = (fs::path {utf_utils::from_utf8(e.steamvr_runtime)} / L"bin" / L"win64" / L"vrserver.exe").wstring();
        }
      }
      return img;
    }

    bool steam_logged_in() {
      return !viple::steam::read_active_user_id3().empty();
    }

    bool launch_steamvr() {
      if (!console_user_present()) {
        return false;
      }
      platf::open_url("steam://rungameid/250820");
      return true;
    }

    bool launch_vr_app(const std::string &url) {
      static const std::regex ok_url {R"(^steam://(launch/\d{1,10}/VR|rungameid/\d{1,10})$)"};
      if (!std::regex_match(url, ok_url) || !console_user_present()) {
        return false;
      }
      platf::open_url(url);
      return true;
    }

    quit_e quit_steamvr(bool allow_kill) {
      const DWORD pid = vrserver_pid();
      if (!pid) {
        return quit_e::not_running;
      }
      // §D.8 第 2 步：結束主控台 session 內的 vrmonitor（WM_CLOSE 實測 30 s 內關不掉）→ vrserver 約 1 s 內 Graceful exit
      const int n = terminate_all(L"vrmonitor.exe");
      BOOST_LOG(info) << "[VIPLE-VR-ORCH] step=quit-vrmonitor terminated=" << n << " vrserverPid=" << pid;
      if (wait_pid_exit(pid, 15s)) {
        return quit_e::exited;
      }
      if (!allow_kill) {
        return quit_e::failed;
      }
      BOOST_LOG(warning) << "[VIPLE-VR-ORCH] steamvr-kill";
      terminate_all(L"vrcompositor.exe");
      terminate_all(L"vrserver.exe");
      return wait_pid_exit(pid, 5s) ? quit_e::killed : quit_e::failed;
    }

    run_result_t run_vrpathreg(const std::string &verb, const std::string &path_utf8) {
      run_result_t r;
      if (verb != "adddriver" && verb != "removedriver") {
        return r;
      }
      const auto e = cached_environment();
      if (e.steamvr_runtime.empty() || path_utf8.find('"') != std::string::npos) {
        return r;
      }
      const fs::path bin = fs::path {utf_utils::from_utf8(e.steamvr_runtime)} / L"bin" / L"win64";
      const std::string cmd = std::format("\"{}\" {} \"{}\"", utf_utils::to_utf8((bin / L"vrpathreg.exe").wstring()), verb, path_utf8);
      boost::filesystem::path wd {bin.wstring()};
      std::error_code ec;
      auto env = boost::this_process::environment();
      auto child = platf::run_command(false, false, cmd, wd, env, nullptr, ec, nullptr);
      if (ec) {
        BOOST_LOG(warning) << "[VIPLE-VR-ORCH] vrpathreg start failed: " << ec.message();
        return r;
      }
      r.started = true;
      HANDLE h = child.native_handle();
      if (WaitForSingleObject(h, 15000) != WAIT_OBJECT_0) {
        r.timed_out = true;
        TerminateProcess(h, 1);
      }
      DWORD code = 0;
      if (GetExitCodeProcess(h, &code)) {
        r.exit_code = static_cast<int>(code);
      }
      child.detach();
      return r;
    }

    std::optional<std::vector<std::string>> registered_viplestream_drivers() {
      auto o = read_openvrpaths();
      if (!o) {
        return std::nullopt;
      }
      std::vector<std::string> out;
      for (const auto &d : o->external_drivers) {
        // 以目錄名判定（manifest name == "viplestream" 的目錄一律叫 viplestream；偏差：不讀每個 manifest）
        if (lower(fs::path {d}.make_preferred().lexically_normal().filename().wstring()) == L"viplestream" ||
            lower(fs::path {d}.make_preferred().lexically_normal().parent_path().filename().wstring()) == L"viplestream") {
          out.push_back(utf_utils::to_utf8(d));
        }
      }
      return out;
    }

    int remove_other_registrations(const std::string &keep_dir_utf8) {
      const auto regs = registered_viplestream_drivers();
      if (!regs) {
        return -1;
      }
      int n = 0;
      const fs::path keep {utf_utils::from_utf8(keep_dir_utf8)};
      for (const auto &r : *regs) {
        if (!keep_dir_utf8.empty() && same_path(fs::path {utf_utils::from_utf8(r)}, keep)) {
          continue;
        }
        const auto rr = run_vrpathreg("removedriver", r);
        BOOST_LOG(info) << "[VIPLE-VR-ORCH] step=unregister-other path=" << tail(fs::path {utf_utils::from_utf8(r)}.parent_path()) << " started=" << rr.started << " rc=" << rr.exit_code;
        ++n;
      }
      return n;
    }

    deploy_result_t deploy_driver() {
      deploy_result_t r;
      auto refuse = [&](std::string reason, std::string level) {
        r.ok = false;
        r.reason = std::move(reason);
        r.level = std::move(level);
        BOOST_LOG(error) << "[VIPLE-VR-ORCH] deploy-refused reason=" << r.reason << " level=" << r.level;
        return r;
      };
      const auto inst = install_dir();
      const auto src_root = inst / L"steamvr";
      const auto src = src_root / L"viplestream";
      if (auto bad = check_chain(src)) {
        return refuse(bad->first == "missing" ? "source" : bad->first, bad->second);
      }
      r.version = read_version_file();
      static const std::regex ver_re {R"(^(\d+\.\d+\.\d+)-h([0-9a-f]{8})$)"};
      std::smatch m;
      if (!std::regex_match(r.version, m, ver_re) || m[1].str() != PROJECT_VERSION) {
        return refuse("version", "viplestream.version");
      }
      const auto manifest = src_root / L"viplestream.sha256";
      const auto mhash = self_update::sha256_file(manifest);
      if (mhash.size() < 8 || mhash.substr(0, 8) != m[2].str()) {
        return refuse("hash", "viplestream.sha256");
      }
      struct entry_t {
        std::string sha;
        std::string rel;
      };
      std::vector<entry_t> entries;
      {
        std::ifstream in(manifest, std::ios::binary);
        std::string line;
        static const std::regex line_re {R"(^([0-9a-f]{64})  viplestream/([A-Za-z0-9_.\-/]+)$)"};
        while (std::getline(in, line)) {
          if (!line.empty() && line.back() == '\r') {
            line.pop_back();
          }
          if (line.empty()) {
            continue;
          }
          std::smatch lm;
          if (!std::regex_match(line, lm, line_re) || lm[2].str().find("..") != std::string::npos) {
            return refuse("hash", "manifest-line");
          }
          entries.push_back({lm[1].str(), lm[2].str()});
        }
      }
      if (entries.empty()) {
        return refuse("hash", "manifest-empty");
      }
      for (const auto &en : entries) {
        if (self_update::sha256_file(src / utf_utils::from_utf8(en.rel)) != en.sha) {
          return refuse("hash", "src/" + en.rel);
        }
      }

      // 目的地：config（安裝目錄規則）→ config\steamvr、guard、<ver>、<ver>\viplestream…（protected）
      if (auto bad = check_chain(inst / L"config")) {
        return refuse(bad->first, bad->second);
      }
      const auto root = state_root();
      const auto ver_dir = root / utf_utils::from_utf8(r.version);
      const auto dst = ver_dir / L"viplestream";
      for (const auto &d : {root, root / L"guard", ver_dir, dst}) {
        if (auto bad = ensure_protected_dir(d)) {
          return refuse(*bad, tail(d));
        }
      }
      bool all_same = true;
      for (const auto &en : entries) {
        std::error_code ec;
        const auto f = dst / utf_utils::from_utf8(en.rel);
        if (!fs::is_regular_file(f, ec) || self_update::sha256_file(f) != en.sha) {
          all_same = false;
          break;
        }
      }
      if (all_same) {
        r.reused = true;
      } else {
        for (const auto &en : entries) {
          const auto rel = fs::path {utf_utils::from_utf8(en.rel)}.make_preferred();
          fs::path cur = dst;
          for (auto it = rel.begin(); it != rel.end() && std::next(it) != rel.end(); ++it) {
            cur /= *it;
            if (auto bad = ensure_protected_dir(cur)) {
              return refuse(*bad, tail(cur));
            }
          }
          const auto to = dst / rel;
          if (!CopyFileW((src / rel).c_str(), to.c_str(), FALSE)) {
            return refuse(GetLastError() == ERROR_SHARING_VIOLATION ? "stale-in-use" : "io", en.rel);
          }
          if (self_update::sha256_file(to) != en.sha) {
            return refuse("hash", "dst/" + en.rel);
          }
        }
      }
      if (auto bad = check_chain(dst)) {
        return refuse(bad->first, bad->second);
      }
      std::string fv;
      if (!platf::getFileVersionInfo(dst / L"bin" / L"win64" / L"driver_viplestream.dll", fv) || fv != std::string {PROJECT_VERSION} + ".0") {
        return refuse("version", "driver_viplestream.dll=" + fv);
      }
      r.ok = true;
      r.driver_dir = utf_utils::to_utf8(dst.wstring());
      return r;
    }

    void prune_old_versions(const std::string &keep_version) {
      static const std::regex ver_re {R"(^\d+\.\d+\.\d+-h[0-9a-f]{8}$)"};
      std::error_code ec;
      for (const auto &d : fs::directory_iterator(state_root(), ec)) {
        const auto name = utf_utils::to_utf8(d.path().filename().wstring());
        if (name == keep_version || !std::regex_match(name, ver_re) || !d.is_directory(ec) || d.is_symlink(ec)) {
          continue;
        }
        std::error_code rm;
        fs::remove_all(d.path(), rm);
        BOOST_LOG(info) << "[VIPLE-VR-ORCH] step=prune ver=" << name << " result=" << (rm ? "kept(in-use)" : "removed");
      }
    }

    // ── guard ────────────────────────────────────────────────────────────
    guard_report_t guard_apply() {
      guard_report_t g;
      if (vrserver_pid()) {
        g.result = guard_e::vrserver_running;
        return g;
      }
      auto u = console_user();
      if (!u) {
        g.result = guard_e::no_user;
        return g;
      }
      const auto vs = vrsettings_path();
      if (!vs) {
        g.detail = "no vrsettings path";
        return g;
      }
      nlohmann::ordered_json j;
      std::string raw;
      const auto lr = load_vrsettings(*vs, j, raw);
      if (lr == vs_e::read_fail || lr == vs_e::parse_fail) {
        g.detail = lr == vs_e::parse_fail ? "parse" : "read";
        return g;  // 解析失敗絕不覆蓋
      }
      if (auto en = get_key(j, "driver_viplestream", "enable"); en && en->is_boolean() && !en->get<bool>()) {
        g.user_disabled_driver = true;
        g.result = guard_e::ok;
        g.detail = "driver_viplestream.enable=false";
        return g;
      }
      bool safe_mode = false;
      if (auto sm = get_key(j, "driver_viplestream", "blocked_by_safe_mode"); sm && sm->is_boolean() && sm->get<bool>()) {
        safe_mode = true;
        g.safe_mode_blocked = true;
        j["driver_viplestream"].erase("blocked_by_safe_mode");  // 不列入 guard 還原
      }

      const auto mpath = marker_path(u->sid);
      auto marker = read_marker(mpath);
      if (!marker) {
        nlohmann::json keys = nlohmann::json::array();
        for (const auto &k : guard_keys()) {
          const bool sec = j.contains(k.section) && j[k.section].is_object();
          const auto old = get_key(j, k.section, k.key);
          keys.push_back({{"section", k.section}, {"key", k.key}, {"section_existed", sec}, {"existed", old.has_value()}, {"old", old ? nlohmann::json::parse(old->dump()) : nlohmann::json()}, {"wrote", k.value}});
        }
        marker = nlohmann::json {{"v", 1}, {"sid", utf_utils::to_utf8(u->sid)}, {"vrsettings", utf_utils::to_utf8(vs->wstring())}, {"file_existed", lr == vs_e::ok}, {"driver_ver", read_version_file()}, {"applied_utc", utc_now()}, {"state", "applying"}, {"keys", keys}};
        if (auto bad = ensure_protected_dir(state_root()); bad || ensure_protected_dir(state_root() / L"guard")) {
          g.detail = "marker dir";
          return g;
        }
        if (!write_system_file(mpath, marker->dump(2))) {
          g.detail = "marker write";
          return g;
        }
      }
      bool changed = safe_mode;
      for (const auto &k : guard_keys()) {
        const auto cur = get_key(j, k.section, k.key);
        if (!cur || nlohmann::json::parse(cur->dump()) != k.value) {
          if (!j.contains(k.section) || !j[k.section].is_object()) {
            j[k.section] = nlohmann::ordered_json::object();
          }
          j[k.section][k.key] = nlohmann::ordered_json::parse(k.value.dump());
          changed = true;
        }
      }
      if (changed) {
        std::string why;
        if (!store_vrsettings(*vs, j, lr == vs_e::ok ? &raw : nullptr, why)) {
          g.detail = "write: " + why;
          return g;
        }
        // 重讀驗證
        nlohmann::ordered_json v;
        std::string raw2;
        if (load_vrsettings(*vs, v, raw2) != vs_e::ok) {
          g.detail = "verify read";
          return g;
        }
        for (const auto &k : guard_keys()) {
          const auto cur = get_key(v, k.section, k.key);
          if (!cur || nlohmann::json::parse(cur->dump()) != k.value) {
            g.detail = std::format("verify {}.{}", k.section, k.key);
            return g;
          }
        }
      }
      (*marker)["state"] = "applied";
      write_system_file(mpath, marker->dump(2));
      if (safe_mode) {
        BOOST_LOG(warning) << "[VIPLE-VR-ORCH] safe-mode unblocked ver=" << read_version_file();
      }
      g.result = guard_e::ok;
      g.detail = changed ? "applied" : "already-applied";
      return g;
    }

    guard_report_t guard_restore() {
      guard_report_t g;
      auto u = console_user();
      if (!u) {
        g.result = guard_pending() ? guard_e::no_user : guard_e::nothing_to_do;
        return g;
      }
      const auto mpath = marker_path(u->sid);
      auto marker = read_marker(mpath);
      if (!marker) {
        std::error_code ec;
        g.result = fs::exists(mpath, ec) ? guard_e::failed : guard_e::nothing_to_do;
        g.detail = g.result == guard_e::failed ? "marker parse" : "";
        return g;
      }
      if (vrserver_pid()) {
        g.result = guard_e::vrserver_running;
        return g;
      }
      const fs::path vs {utf_utils::from_utf8(marker->value("vrsettings", ""))};
      const auto expect = vrsettings_path();
      if (vs.empty() || !expect || !same_path(vs, *expect)) {
        g.detail = "vrsettings path changed";
        return g;
      }
      nlohmann::ordered_json j;
      std::string raw;
      const auto lr = load_vrsettings(vs, j, raw);
      if (lr == vs_e::read_fail || lr == vs_e::parse_fail) {
        g.detail = lr == vs_e::parse_fail ? "parse" : "read";
        return g;
      }
      bool changed = false;
      std::string skipped;
      for (const auto &k : (*marker)["keys"]) {
        const auto sec = k.value("section", "");
        const auto key = k.value("key", "");
        const auto cur = get_key(j, sec.c_str(), key.c_str());
        if (!cur || nlohmann::json::parse(cur->dump()) != k["wrote"]) {
          skipped += " " + sec + "." + key;
          BOOST_LOG(info) << "[VIPLE-VR-ORCH] guard-restore skipped key=" << sec << "." << key << " reason=changed";
          continue;
        }
        if (k.value("existed", false)) {
          j[sec][key] = nlohmann::ordered_json::parse(k["old"].dump());
        } else {
          j[sec].erase(key);
          if (!k.value("section_existed", true) && j[sec].empty()) {
            j.erase(sec);
          }
        }
        changed = true;
      }
      if (changed) {
        std::string why;
        if (!store_vrsettings(vs, j, nullptr, why)) {
          g.detail = "write: " + why;
          return g;
        }
      }
      std::error_code ec;
      fs::remove(mpath, ec);
      g.result = guard_e::ok;
      g.detail = changed ? "restored" + (skipped.empty() ? ""s : " skipped:" + skipped) : "unchanged" + (skipped.empty() ? ""s : " skipped:" + skipped);
      return g;
    }

    bool guard_pending() {
      std::error_code ec;
      for (const auto &d : fs::directory_iterator(state_root() / L"guard", ec)) {
        if (d.path().extension() == L".json") {
          return true;
        }
      }
      return false;
    }

    std::string guard_keys_snapshot() {
      const auto vs = vrsettings_path();
      if (!vs) {
        return "no-path";
      }
      nlohmann::ordered_json j;
      std::string raw;
      const auto lr = load_vrsettings(*vs, j, raw);
      if (lr == vs_e::read_fail || lr == vs_e::parse_fail) {
        return "unreadable";
      }
      std::string out;
      for (const auto &[sec, key] : {std::pair {"steamvr", "forcedDriver"}, std::pair {"driver_vrlink", "enable"}, std::pair {"driver_viplestream", "enable"}, std::pair {"driver_viplestream", "blocked_by_safe_mode"}}) {
        const auto v = get_key(j, sec, key);
        out += std::format("{}{}.{}={}", out.empty() ? "" : " ", sec, key, v ? v->dump() : "<absent>");
      }
      return out;
    }

    std::string steamvr_log_tail(const std::string &file, size_t cap) {
      const auto root = cached_environment().steam_root;
      if (root.empty() || file.find_first_of("\\/:") != std::string::npos) {
        return {};
      }
      std::string out;
      if (read_user_file(fs::path {utf_utils::from_utf8(root)} / L"logs" / utf_utils::from_utf8(file), cap, true, out) != rd_e::ok) {
        out.clear();
      }
      return out;
    }

    // ── 衝突 ─────────────────────────────────────────────────────────────
    conflict_t detect_conflicts(bool driver_connected, uint32_t other_hmd) {
      conflict_t c;
      const auto env = cached_environment();
      c.openxr_other = env.openxr_other;
      c.steamvr_running = vrserver_pid() != 0;
      if (auto u = console_user()) {
        const auto id = reg_dword(HKEY_USERS, u->sid + L"\\Software\\Valve\\Steam", L"RunningAppID");
        if (id && *id != 0) {
          const auto ids = vr_manifest_app_ids(false);
          c.vr_app_running = ids.contains(std::to_string(*id));
        }
      }
      if (driver_connected && other_hmd == VRIPC_OTHER_HMD_PRESENT) {
        c.kind = "other-hmd";
      } else if (c.steamvr_running && !driver_connected) {
        // 我們的 driver 沒在這個 vrserver 載入 → 一定要重啟；「HMD 啟用」的 vrserver.txt 行樣式未定案（U25），保守視為 vrlink
        c.kind = "vrlink";
      } else if (c.vr_app_running) {
        c.kind = "vr-app";
      } else if (!console_procs(L"VirtualDesktop.Streamer.exe").empty()) {
        c.kind = "vd-streamer";
      }
      c.any = !c.kind.empty();
      {
        std::lock_guard lk(g_mtx);
        g_conf = c;
      }
      return c;
    }

    conflict_t cached_conflicts() {
      std::lock_guard lk(g_mtx);
      return g_conf;
    }

    std::set<std::string> vr_manifest_app_ids(bool refresh) {
      {
        std::lock_guard lk(g_mtx);
        if (g_vr_ids_loaded && !refresh) {
          return g_vr_ids;
        }
      }
      std::set<std::string> ids;
      auto root = cached_environment().steam_root;
      if (root.empty()) {
        root = utf_utils::to_utf8(reg_sz(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"));
      }
      std::string text;
      if (!root.empty() && read_user_file(fs::path {utf_utils::from_utf8(root)} / L"config" / L"steamapps.vrmanifest", k_cap_4m, false, text) == rd_e::ok) {
        const auto j = nlohmann::json::parse(text, nullptr, false);
        static const std::regex key_re {R"(^steam\.app\.(\d{1,10})$)"};
        static const std::regex url_re {R"(^steam://launch/(\d{1,10})/VR$)"};
        if (!j.is_discarded() && j.is_object() && j.contains("applications") && j["applications"].is_array()) {
          for (const auto &a : j["applications"]) {
            if (!a.is_object() || !a.contains("app_key") || !a["app_key"].is_string() || !a.contains("url") || !a["url"].is_string()) {
              continue;
            }
            std::smatch km, um;
            const auto key = a["app_key"].get<std::string>();
            const auto url = a["url"].get<std::string>();
            if (std::regex_match(key, km, key_re) && std::regex_match(url, um, url_re) && km[1].str() == um[1].str()) {
              ids.insert(km[1].str());
            }
          }
        }
      }
      std::lock_guard lk(g_mtx);
      g_vr_ids = ids;
      g_vr_ids_loaded = true;
      return ids;
    }
  }  // namespace platform
}  // namespace vr
