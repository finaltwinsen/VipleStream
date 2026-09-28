/**
 * @file src/platform/windows/config_acl.cpp
 * @brief VipleStream [VIPLE-SEC]：收緊 config 目錄內機密檔案的 DACL（範圍與理由見 config_acl.h）。
 */
// standard includes
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// platform includes
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>

// local includes
#include "config_acl.h"
#include "src/logging.h"

using namespace std::literals;

namespace platf::config_acl {
  namespace {
    // 目標 SD：owner／group = SYSTEM，protected DACL 只有 SYSTEM 與 Administrators 完全控制（沒有 Users、
    // 也沒有 ALL APPLICATION PACKAGES）。與 tools/sunshinesvc.cpp 建立 viplestream-svc.log 的 SDDL 相同。
    constexpr const wchar_t *kSddlFull = L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)";
    // 提升的管理員不能把 owner 設成 SYSTEM（需要 SeRestorePrivilege），只設 DACL。
    constexpr const wchar_t *kSddlDaclOnly = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)";
    // credentials\ 往下走的深度上限（實際只有一層：cakey.pem／cacert.pem）
    constexpr int kCredentialsMaxDepth = 8;

    enum class identity_e {
      system,  ///< token 使用者是 LocalSystem
      elevated_admin,  ///< BUILTIN\Administrators 在 token 裡是 enabled（UAC 提升或 SSH）
      other,
    };

    enum class kind_e {
      conf,
      conf_bak,
      log,
      svc_log,
      credential,
      state,  ///< Web UI 帳密與配對狀態（sunshine_state.json 或 credentials_file／file_state 指定的檔）
    };

    enum class outcome_e {
      tightened,  ///< 這次改了 SD
      compliant,  ///< 本來就合規
      refused,  ///< 刻意不處理（reparse point、hard link、目錄）
      failed,  ///< 開檔／查詢／設定失敗
    };

    struct result_t {
      outcome_e outcome;
      const char *reason;
      DWORD err;
    };

    struct stats_t {
      int tightened = 0;
      int skipped = 0;
    };

    // prepare_log_file 的結果：它在 logging::init() 之前執行、不能寫 log，
    // 交給 tighten_config_dir 計入統計（兩者都在啟動時的主執行緒，不需要鎖）
    std::wstring g_prepared_path;  // 小寫後的完整路徑
    bool g_prepared_changed = false;

    class handle_guard_t {
    public:
      explicit handle_guard_t(HANDLE handle):
          handle_ {handle} {
      }

      ~handle_guard_t() {
        if (handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr) {
          CloseHandle(handle_);
        }
      }

      handle_guard_t(const handle_guard_t &) = delete;
      handle_guard_t &operator=(const handle_guard_t &) = delete;

      HANDLE get() const {
        return handle_;
      }

    private:
      HANDLE handle_;
    };

    class local_sd_guard_t {
    public:
      explicit local_sd_guard_t(PSECURITY_DESCRIPTOR sd):
          sd_ {sd} {
      }

      ~local_sd_guard_t() {
        if (sd_ != nullptr) {
          LocalFree(sd_);
        }
      }

      local_sd_guard_t(const local_sd_guard_t &) = delete;
      local_sd_guard_t &operator=(const local_sd_guard_t &) = delete;

    private:
      PSECURITY_DESCRIPTOR sd_;
    };

    // 目標 SD 與拆出來的 owner／group／DACL（指向 sd_ 內部，sd_ 活著時有效）
    class target_sd_t {
    public:
      explicit target_sd_t(bool full):
          full_ {full} {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(full ? kSddlFull : kSddlDaclOnly, SDDL_REVISION_1, &sd_, nullptr)) {
          sd_ = nullptr;
          return;
        }
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        bool ok = GetSecurityDescriptorDacl(sd_, &present, &dacl_, &defaulted) && present && dacl_ != nullptr;
        if (ok && full) {
          ok = GetSecurityDescriptorOwner(sd_, &owner_, &defaulted) && owner_ != nullptr &&
               GetSecurityDescriptorGroup(sd_, &group_, &defaulted) && group_ != nullptr;
        }
        if (!ok) {
          LocalFree(sd_);
          sd_ = nullptr;
        }
      }

      ~target_sd_t() {
        if (sd_ != nullptr) {
          LocalFree(sd_);
        }
      }

      target_sd_t(const target_sd_t &) = delete;
      target_sd_t &operator=(const target_sd_t &) = delete;

      bool valid() const {
        return sd_ != nullptr;
      }

      bool full() const {
        return full_;
      }

      PSECURITY_DESCRIPTOR sd() const {
        return sd_;
      }

      PSID owner() const {
        return owner_;
      }

      PSID group() const {
        return group_;
      }

      PACL dacl() const {
        return dacl_;
      }

    private:
      bool full_;
      PSECURITY_DESCRIPTOR sd_ = nullptr;
      PSID owner_ = nullptr;
      PSID group_ = nullptr;
      PACL dacl_ = nullptr;
    };

    std::wstring ascii_lower(std::wstring_view text) {
      std::wstring out;
      out.reserve(text.size());
      for (const wchar_t ch : text) {
        out.push_back((ch >= L'A' && ch <= L'Z') ? static_cast<wchar_t>(ch - L'A' + L'a') : ch);
      }
      return out;
    }

    // 正規化成反斜線、去掉結尾分隔符號（磁碟根目錄除外）
    std::wstring normalized(const std::filesystem::path &path) {
      std::wstring out = path.lexically_normal().native();
      for (auto &ch : out) {
        if (ch == L'/') {
          ch = L'\\';
        }
      }
      while (out.size() > 3 && out.back() == L'\\') {
        out.pop_back();
      }
      return out;
    }

    const char *kind_name(kind_e kind) {
      switch (kind) {
        case kind_e::conf:
          return "conf";
        case kind_e::conf_bak:
          return "conf-bak";
        case kind_e::log:
          return "log";
        case kind_e::svc_log:
          return "svc-log";
        case kind_e::credential:
          return "credential";
        case kind_e::state:
          return "state";
      }
      return "unknown";
    }

    identity_e current_identity() {
      HANDLE raw_token = nullptr;
      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return identity_e::other;
      }
      handle_guard_t token {raw_token};

      alignas(TOKEN_USER) BYTE buffer[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE] {};
      DWORD length = 0;
      if (GetTokenInformation(token.get(), TokenUser, buffer, sizeof(buffer), &length)) {
        const auto *user = reinterpret_cast<const TOKEN_USER *>(buffer);
        if (IsWellKnownSid(user->User.Sid, WinLocalSystemSid)) {
          return identity_e::system;
        }
      }

      // UAC 過濾後的 token 裡 BA 是 deny-only，CheckTokenMembership 會回 FALSE；提升的管理員（含 SSH）才是 TRUE
      BYTE admins[SECURITY_MAX_SID_SIZE] {};
      DWORD admins_size = sizeof(admins);
      BOOL member = FALSE;
      if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &admins_size) &&
          CheckTokenMembership(nullptr, admins, &member) && member) {
        return identity_e::elevated_admin;
      }
      return identity_e::other;
    }

    // DACL 是否正好是 protected 的 {SYSTEM:FA, Administrators:FA}（非繼承、順序不拘）
    bool is_protected_sy_ba(PSECURITY_DESCRIPTOR sd, PACL dacl) {
      SECURITY_DESCRIPTOR_CONTROL control = 0;
      DWORD revision = 0;
      if (sd == nullptr || !GetSecurityDescriptorControl(sd, &control, &revision) || !(control & SE_DACL_PROTECTED)) {
        return false;
      }
      if (dacl == nullptr) {
        // NULL DACL＝所有人完全控制
        return false;
      }
      ACL_SIZE_INFORMATION acl_info {};
      if (!GetAclInformation(dacl, &acl_info, sizeof(acl_info), AclSizeInformation) || acl_info.AceCount != 2) {
        return false;
      }
      bool have_system = false;
      bool have_admins = false;
      for (DWORD i = 0; i < acl_info.AceCount; ++i) {
        void *raw_ace = nullptr;
        if (!GetAce(dacl, i, &raw_ace)) {
          return false;
        }
        const auto *header = static_cast<const ACE_HEADER *>(raw_ace);
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERITED_ACE)) {
          return false;
        }
        auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw_ace);
        if (ace->Mask != FILE_ALL_ACCESS) {
          return false;
        }
        PSID sid = &ace->SidStart;
        if (IsWellKnownSid(sid, WinLocalSystemSid)) {
          have_system = true;
        } else if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) {
          have_admins = true;
        } else {
          return false;
        }
      }
      return have_system && have_admins;
    }

    // 以 handle 查驗並設定，避免「檢查的是 A、設定的是被換掉的 B」。
    // FILE_FLAG_OPEN_REPARSE_POINT：symlink／junction 開到的是它自己，接著以屬性拒絕，絕不跟隨。
    result_t secure_existing_file(const std::wstring &path, const target_sd_t &target) {
      const DWORD access = READ_CONTROL | WRITE_DAC | (target.full() ? WRITE_OWNER : 0);
      handle_guard_t file {CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
      if (file.get() == INVALID_HANDLE_VALUE) {
        return {outcome_e::failed, "open", GetLastError()};
      }

      BY_HANDLE_FILE_INFORMATION file_info {};
      if (!GetFileInformationByHandle(file.get(), &file_info)) {
        return {outcome_e::failed, "info", GetLastError()};
      }
      if (file_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        return {outcome_e::refused, "reparse", 0};
      }
      if (file_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        return {outcome_e::refused, "directory", 0};
      }
      if (file_info.nNumberOfLinks > 1) {
        // 另一個名字可能在 config 之外；改它的 SD 等於改別處的檔案
        return {outcome_e::refused, "hardlink", 0};
      }

      PSID owner = nullptr;
      PACL dacl = nullptr;
      PSECURITY_DESCRIPTOR current = nullptr;
      DWORD status = GetSecurityInfo(file.get(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &current);
      if (status != ERROR_SUCCESS) {
        return {outcome_e::failed, "query", status};
      }
      bool compliant = false;
      {
        local_sd_guard_t current_guard {current};
        compliant = is_protected_sy_ba(current, dacl) &&
                    (!target.full() || (owner != nullptr && IsWellKnownSid(owner, WinLocalSystemSid)));
      }
      if (compliant) {
        return {outcome_e::compliant, "", 0};
      }

      SECURITY_INFORMATION info_bits = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
      if (target.full()) {
        info_bits |= OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION;
      }
      status = SetSecurityInfo(file.get(), SE_FILE_OBJECT, info_bits, target.full() ? target.owner() : nullptr, target.full() ? target.group() : nullptr, target.dacl(), nullptr);
      if (status != ERROR_SUCCESS) {
        return {outcome_e::failed, "set", status};
      }
      return {outcome_e::tightened, "", 0};
    }

    void account(stats_t &stats, kind_e kind, const result_t &result) {
      switch (result.outcome) {
        case outcome_e::tightened:
          ++stats.tightened;
          return;
        case outcome_e::compliant:
          ++stats.skipped;
          return;
        case outcome_e::refused:
        case outcome_e::failed:
          ++stats.skipped;
          // 只印類別與原因：不印路徑、檔名（credentials 的檔名本身就指向私鑰）
          BOOST_LOG(warning) << "[VIPLE-SEC] config-acl skip kind="sv << kind_name(kind) << " reason="sv << result.reason << " err="sv << result.err;
          return;
      }
    }

    // 頂層檔名分類（不分大小寫）；不在範圍內回 false。
    // state_names：credentials_file／file_state 位於 config 頂層時的實際檔名（已轉小寫）。
    bool classify_top_level(std::wstring_view name, const std::vector<std::wstring> &state_names, kind_e &kind) {
      const std::wstring lower = ascii_lower(name);
      if (lower == L"sunshine.conf"sv) {
        kind = kind_e::conf;
        return true;
      }
      // Web UI 帳號、salt、密碼雜湊（單次 SHA-256，可離線暴力破解）與配對狀態；只有 SYSTEM server 讀寫
      if (lower == L"sunshine_state.json"sv || std::find(state_names.begin(), state_names.end(), lower) != state_names.end()) {
        kind = kind_e::state;
        return true;
      }
      // sunshine*.conf*（sunshine.conf 以外）一律視為 conf 備份：sunshine.conf.bak、.bak-<ts>、.bak.<ts>、
      // .old、.orig，以及 Explorer 複製出來的「sunshine - Copy.conf」「sunshine - 複製.conf」。
      // 放在 log 規則之前；程式本身不會產生其他 sunshine*.conf* 檔。
      if (lower.starts_with(L"sunshine"sv) && lower.find(L".conf"sv, 8) != std::wstring::npos) {
        kind = kind_e::conf_bak;
        return true;
      }
      if (lower == L"viplestream-svc.log"sv) {
        kind = kind_e::svc_log;
        return true;
      }
      // sunshine*.log*：「sunshine」之後任何位置出現「.log」（sunshine.log、sunshine-cli.log、sunshine-pre-x.log…）
      if (lower.starts_with(L"sunshine"sv) && lower.find(L".log"sv, 8) != std::wstring::npos) {
        kind = kind_e::log;
        return true;
      }
      return false;
    }

    // 列舉目錄（不遞迴、不跟隨）；回 false 表示目錄打不開
    template<class Fn>
    bool for_each_entry(const std::wstring &dir, Fn &&fn) {
      WIN32_FIND_DATAW find_data {};
      const std::wstring pattern = dir + L"\\*";
      HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &find_data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
      if (find == INVALID_HANDLE_VALUE) {
        return false;
      }
      do {
        const std::wstring_view name {find_data.cFileName};
        if (name == L"."sv || name == L".."sv) {
          continue;
        }
        fn(name, find_data.dwFileAttributes);
      } while (FindNextFileW(find, &find_data));
      FindClose(find);
      return true;
    }

    void walk_credentials(const std::wstring &dir, int depth, const target_sd_t &target, stats_t &stats) {
      const bool listed = for_each_entry(dir, [&](std::wstring_view name, DWORD attrs) {
        const std::wstring path = dir + L"\\" + std::wstring {name};
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
          account(stats, kind_e::credential, {outcome_e::refused, "reparse", 0});
          return;
        }
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
          // 只收緊檔案，子目錄自己的 DACL 不動
          if (depth < kCredentialsMaxDepth) {
            walk_credentials(path, depth + 1, target, stats);
          } else {
            account(stats, kind_e::credential, {outcome_e::refused, "depth", 0});
          }
          return;
        }
        account(stats, kind_e::credential, secure_existing_file(path, target));
      });
      if (!listed && GetLastError() != ERROR_FILE_NOT_FOUND) {
        account(stats, kind_e::credential, {outcome_e::failed, "list", GetLastError()});
      }
    }

    // sunshine.conf 是否已經被 SYSTEM service 收緊過（＝這個 config 目錄由 service 管理）
    bool conf_is_protected(const std::wstring &dir) {
      const std::wstring path = dir + L"\\sunshine.conf";
      handle_guard_t file {CreateFileW(path.c_str(), READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
      if (file.get() == INVALID_HANDLE_VALUE) {
        return false;
      }
      PACL dacl = nullptr;
      PSECURITY_DESCRIPTOR sd = nullptr;
      if (GetSecurityInfo(file.get(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
      }
      local_sd_guard_t sd_guard {sd};
      return is_protected_sy_ba(sd, dacl);
    }
  }  // namespace

  void prepare_log_file(const std::filesystem::path &config_dir, const std::filesystem::path &log_file) {
    const std::wstring dir = normalized(config_dir);
    const std::wstring parent = normalized(log_file.parent_path());
    const std::wstring file_name = log_file.filename().native();
    if (dir.empty() || file_name.empty() || ascii_lower(parent) != ascii_lower(dir)) {
      // log_path 設到 config 目錄以外：那是使用者自己選的位置，不動
      return;
    }

    bool full = false;
    switch (current_identity()) {
      case identity_e::system:
        full = true;
        break;
      case identity_e::elevated_admin:
        if (!conf_is_protected(dir)) {
          return;
        }
        break;
      case identity_e::other:
        return;
    }

    const target_sd_t target {full};
    if (!target.valid()) {
      return;
    }

    const std::wstring path = dir + L"\\" + file_name;
    bool changed = false;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
      if (GetLastError() != ERROR_FILE_NOT_FOUND) {
        return;
      }
      // 以目標 SD 直接建立：之後 logging::init() 的截斷式開檔（CREATE_ALWAYS）會保留這個 SD
      SECURITY_ATTRIBUTES security_attributes {sizeof(security_attributes), target.sd(), FALSE};
      HANDLE created = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security_attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (created != INVALID_HANDLE_VALUE) {
        CloseHandle(created);
        changed = true;
      } else if (GetLastError() != ERROR_FILE_EXISTS) {
        // 建不了（例如權限不足）：交給 logging::init() 照舊處理
        return;
      }
    }
    if (!changed) {
      // 已存在：就地收緊（冪等）；截斷不會改變 SD
      changed = secure_existing_file(path, target).outcome == outcome_e::tightened;
    }

    if (full) {
      g_prepared_path = ascii_lower(path);
      g_prepared_changed = changed;
    }
  }

  void tighten_config_dir(const std::filesystem::path &config_dir, std::span<const std::filesystem::path> extra_secret_files) {
    if (current_identity() != identity_e::system) {
      // console 模式（管理員或一般使用者）不改既有安裝的 ACL：非 SYSTEM 的 server 可能要靠 Users 讀這些檔
      BOOST_LOG(debug) << "[VIPLE-SEC] config-acl not running as SYSTEM; skipped"sv;
      return;
    }

    const std::wstring dir = normalized(config_dir);
    const DWORD dir_attrs = GetFileAttributesW(dir.c_str());
    if (dir_attrs == INVALID_FILE_ATTRIBUTES || !(dir_attrs & FILE_ATTRIBUTE_DIRECTORY) || (dir_attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
      BOOST_LOG(warning) << "[VIPLE-SEC] config-acl skipped reason=config-dir attrs="sv << dir_attrs;
      return;
    }

    const target_sd_t target {true};
    if (!target.valid()) {
      BOOST_LOG(warning) << "[VIPLE-SEC] config-acl skipped reason=sd err="sv << GetLastError();
      return;
    }

    // credentials_file／file_state 改過檔名時一併涵蓋；只收位於 config 頂層的（規則同 prepare_log_file 對
    // log_path 的處理：放在 config 以外就是使用者自選的位置，不動）
    const std::wstring dir_lower = ascii_lower(dir);
    std::vector<std::wstring> state_names;
    for (const auto &file : extra_secret_files) {
      if (file.empty() || !file.has_filename()) {
        continue;
      }
      if (ascii_lower(normalized(file.parent_path())) != dir_lower) {
        continue;
      }
      state_names.push_back(ascii_lower(file.filename().native()));
    }

    stats_t stats;
    const bool listed = for_each_entry(dir, [&](std::wstring_view name, DWORD attrs) {
      if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        // credentials\ 在下面另外處理；其他目錄（steamvr、covers、logs…）一律不碰
        return;
      }
      kind_e kind {};
      if (!classify_top_level(name, state_names, kind)) {
        return;
      }
      if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        account(stats, kind, {outcome_e::refused, "reparse", 0});
        return;
      }
      const std::wstring path = dir + L"\\" + std::wstring {name};
      auto result = secure_existing_file(path, target);
      if (result.outcome == outcome_e::compliant && g_prepared_changed && ascii_lower(path) == g_prepared_path) {
        // prepare_log_file 在 logging::init() 之前已經收緊（或以目標 SD 建立）的 log 檔
        result.outcome = outcome_e::tightened;
      }
      account(stats, kind, result);
    });
    if (!listed) {
      BOOST_LOG(warning) << "[VIPLE-SEC] config-acl skip kind=dir reason=list err="sv << GetLastError();
    }

    const std::wstring credentials = dir + L"\\credentials";
    const DWORD cred_attrs = GetFileAttributesW(credentials.c_str());
    if (cred_attrs != INVALID_FILE_ATTRIBUTES && (cred_attrs & FILE_ATTRIBUTE_DIRECTORY)) {
      if (cred_attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        account(stats, kind_e::credential, {outcome_e::refused, "reparse", 0});
      } else {
        walk_credentials(credentials, 1, target, stats);
      }
    }

    BOOST_LOG(info) << "[VIPLE-SEC] config-acl tightened files="sv << stats.tightened << " skipped="sv << stats.skipped;
  }

  void publish_webui_port(const std::filesystem::path &config_dir, std::uint16_t port) {
    if (current_identity() != identity_e::system) {
      // 非 SYSTEM 不會收緊 sunshine.conf，`--shortcut` 讀 conf 就拿得到 port，不需要這個檔
      return;
    }

    const std::wstring dir = normalized(config_dir);
    const std::wstring final_path = dir + L"\\" + kWebUiPortFileName;
    const std::wstring temp_path = final_path + L".tmp";

    // 先刪殘留的暫存檔（DeleteFileW 刪的是名字本身，不跟隨 symlink，也不動 hard link 的另一個名字），
    // 再以 CREATE_NEW＋FILE_FLAG_OPEN_REPARSE_POINT 建立：名字又被佔用就放棄，絕不寫到別處。
    // 不給 SECURITY_ATTRIBUTES：刻意沿用 config 目錄繼承的 ACL（Users:RX），讓一般使用者讀得到。
    DeleteFileW(temp_path.c_str());
    const char *step = "create";
    DWORD err = 0;
    {
      handle_guard_t file {CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
      if (file.get() == INVALID_HANDLE_VALUE) {
        err = GetLastError();
      } else {
        const std::string text = std::to_string(port);
        DWORD written = 0;
        if (!WriteFile(file.get(), text.data(), static_cast<DWORD>(text.size()), &written, nullptr) || written != text.size()) {
          step = "write";
          err = GetLastError();
          if (err == 0) {
            err = ERROR_WRITE_FAULT;
          }
        }
      }
    }
    if (err == 0) {
      // 同一個 volume 內的 rename：SD 跟著檔案走；目的地若是 reparse point，換掉的是它本身
      step = "rename";
      if (!MoveFileExW(temp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = GetLastError();
      }
    }
    if (err != 0) {
      DeleteFileW(temp_path.c_str());
      BOOST_LOG(warning) << "[VIPLE-SEC] webui-port publish failed step="sv << step << " err="sv << err;
      return;
    }
    BOOST_LOG(debug) << "[VIPLE-SEC] webui-port published port="sv << port;
  }

  std::optional<int> read_published_webui_port(const std::filesystem::path &config_dir) {
    std::ifstream in {config_dir / kWebUiPortFileName, std::ios::binary};
    if (!in) {
      return std::nullopt;
    }
    // 內容只有十進位數字（publish_webui_port 不寫換行，最多 5 位）；buffer 讀滿就代表內容太長，拒絕
    char buffer[8] {};
    in.read(buffer, sizeof(buffer));
    const auto length = static_cast<std::size_t>(in.gcount());
    int port = 0;
    const auto [end, ec] = std::from_chars(buffer, buffer + length, port);
    if (length == 0 || length == sizeof(buffer) || ec != std::errc {} || end != buffer + length || port <= 0 || port > 65535) {
      return std::nullopt;
    }
    return port;
  }
}  // namespace platf::config_acl
