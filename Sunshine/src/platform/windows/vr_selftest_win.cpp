/**
 * @file src/platform/windows/vr_selftest_win.cpp
 * @brief VipleStream §VR（M1b S1-18、§F.5）：`--vr-selftest` 的 Windows 子測試（T0、T2、T6 的平台部分）。
 *
 * - T0：環境（自身／主控台 session、SYSTEM、主控台使用者的 RID 與 elevationType、DXGI adapter）、
 *   VR pipe（bridge 的）與 admin pipe 的安全描述元：owner=SY、protected DACL、ACE 集合與權限、mandatory label；
 *   VR pipe 另由 bridge 在 pipe_mtx_ 內做一次 SetSecurityInfo 來回（證明 open mode 帶了 WRITE_DAC，K3）。
 * - T2（§F.5）：以主控台使用者身分啟動 `vr_probe --mode ipcpeer`（run_command＋kill-on-close job，sec-M13），
 *   依序驗：E-1 第二 instance 被拒；正常握手（含 shm／event 的 SD、E-2 雙向複製）；server 端消費者
 *   （§B.7 fence 確認＋§B.8 驗證，E-3 延遲；另記錄 peer 不 Flush 的對照）；peer 被殺後的 teardown（E-4）；
 *   RECONFIG 重連；fence 寫成 UINT64_MAX → BYE(DEVICE_LOST)；peer 停止 Signal → 逾時計數與 BYE(PROTOCOL_ERROR)
 *   （E-4）；所有權（sec-M1）；不讀 pipe（sec-M8）；雙寫入者＋echo 驗證（K26、§B.8 第 4 條）；升權嘗試（U30，只記錄）；
 *   ABI 不同 → REJECT(ABI_MISMATCH)；HELLO 大小不符 → REJECT(BAD_MESSAGE)；冒名 → REJECT(IDENTITY)；恢復；
 *   連續 10 次錯誤 HELLO 的退避（sec-m18）與之後的恢復。
 *   V3 起加上 T2.live（display_vr_t live 消費 60 s）；§F.5 另列的 gpu-hold（U31）需要 vr_probe 的 GPU hold，
 *   以 `result=NOT-RUN until=V4` 明列，計入 (final) 行的 notRun=，rc=0 不代表它驗過。
 * - T1（V3）：display_vr_t 探測形態＋VR 形狀的 encoder 探測；T1b（V3）：VR 探測前後 /serverinfo 逐字相同。
 * - T4（V4，只有 --manual-steamvr）：真的 SteamVR driver 握手、HMD Activate、HMD_PRESENTING、selftest consumer，
 *   scene 時讀回 vr_probe 畫在兩眼角落的位元圖案與 descriptor 的 renderPose 比對（mismatch=0）。
 * - T6：QPC 換算（S1-01，含飽和）、§B.8 descriptor 驗證純函式、ABI 表與 vr_probe 逐字比對
 *   （vr_ipc_abi_table.h）、`vr_probe --mode unit`。
 *
 * 與其他模組的接縫集中在 bridge_view（vr_bridge，S1-07）；vr_probe 的命令列與輸出格式見 T2／T6 各函式的註解。
 * log 衛生：不印 handle 值、完整 SID（只印 RID）、GUID。
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <set>
#include <utility>
#include <vector>

// lib includes（boost 要在 Windows.h 之前，理由同 misc.cpp）
#include <boost/filesystem/path.hpp>
#include <boost/process/v1.hpp>
#include <nlohmann/json.hpp>

// platform includes
// clang-format off
#include <fcntl.h>
#include <io.h>
#include <Windows.h>
#include <aclapi.h>
#include <dxgi.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <WtsApi32.h>
#include <ShlObj.h>
#include <winternl.h>  // NTSTATUS（display.h 的 D3DKMT 型別）
// clang-format on

// local includes
#include "display.h"
#include "display_vram.h"
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/video.h"
#include "src/vr/vr_bridge.h"
#include "src/vr/vr_ipc_abi.h"
#include "src/vr/vr_ipc_abi_table.h"
#include "src/vr/vr_scene_pattern.h"
#include "src/vr/vr_selftest.h"
#include <moonlight-common-c/src/VipleVr.h>
#include "utf_utils.h"
#include "vr_admin_pipe.h"

using namespace std::literals;

namespace vr::selftest::platform {
  namespace {
    namespace bp = boost::process::v1;
    using steady = std::chrono::steady_clock;

    // ── 小工具 ──────────────────────────────────────────────────────────

    struct handle_t {
      HANDLE h = nullptr;

      handle_t() = default;

      explicit handle_t(HANDLE v):
          h {v == INVALID_HANDLE_VALUE ? nullptr : v} {
      }

      handle_t(handle_t &&o) noexcept:
          h {std::exchange(o.h, nullptr)} {
      }

      handle_t &operator=(handle_t &&o) noexcept {
        if (this != &o) {
          reset();
          h = std::exchange(o.h, nullptr);
        }
        return *this;
      }

      handle_t(const handle_t &) = delete;
      handle_t &operator=(const handle_t &) = delete;

      ~handle_t() {
        reset();
      }

      void reset() {
        if (h) {
          CloseHandle(h);
          h = nullptr;
        }
      }

      explicit operator bool() const {
        return h != nullptr;
      }
    };

    uint64_t delta(uint64_t now, uint64_t before) {
      return now >= before ? now - before : 0;
    }

    int64_t ms_since(steady::time_point t) {
      return std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - t).count();
    }

    /// 等到 pred() 為真；回傳花了幾 ms，逾時或 stop 回 -1
    int64_t wait_until(std::chrono::milliseconds limit, const std::atomic<bool> &stop, const std::function<bool()> &pred, std::chrono::milliseconds step = 5ms) {
      const auto t0 = steady::now();
      for (;;) {
        if (pred()) {
          return ms_since(t0);
        }
        if (stop.load() || steady::now() - t0 >= limit) {
          return -1;
        }
        std::this_thread::sleep_for(step);
      }
    }

    std::filesystem::path install_dir() {
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
      return std::filesystem::path {buf}.parent_path();
    }

    /// `--probe-path` 或預設的 `<install>\tools\vr_probe\vr_probe.exe`
    std::filesystem::path probe_exe(const request_t &req) {
      if (!req.probe_path.empty()) {
        return std::filesystem::path {utf_utils::from_utf8(req.probe_path)};
      }
      return install_dir() / L"tools" / L"vr_probe" / L"vr_probe.exe";
    }

    /// 回傳 nullopt 表示可用；否則是原因
    std::optional<std::string> probe_unusable(const std::filesystem::path &exe) {
      std::error_code ec;
      if (!exe.is_absolute()) {
        return "probe path is not absolute"s;
      }
      if (!std::filesystem::is_regular_file(exe, ec)) {
        return std::format("vr_probe not found at {} (deploy it by scp, or pass --probe-path)", utf_utils::to_utf8(exe.wstring()));
      }
      auto ext = exe.extension().wstring();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
      if (ext != L".exe") {
        return "probe path must point to an .exe"s;
      }
      return std::nullopt;
    }

    std::string luid_str(LUID l) {
      return std::format("{:08x}:{:08x}", static_cast<uint32_t>(l.HighPart), static_cast<uint32_t>(l.LowPart));
    }

    DWORD sid_rid(PSID sid) {
      if (!sid || !IsValidSid(sid)) {
        return 0;
      }
      const UCHAR n = *GetSidSubAuthorityCount(sid);
      return n ? *GetSidSubAuthority(sid, n - 1) : 0;
    }

    /// S-1-5-5-X-Y
    bool is_logon_sid(PSID sid) {
      if (!sid || !IsValidSid(sid)) {
        return false;
      }
      const SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
      const auto *auth = GetSidIdentifierAuthority(sid);
      return std::equal(std::begin(auth->Value), std::end(auth->Value), std::begin(nt.Value)) &&
             *GetSidSubAuthorityCount(sid) == SECURITY_LOGON_IDS_RID_COUNT && *GetSidSubAuthority(sid, 0) == SECURITY_LOGON_IDS_RID;
    }

    std::vector<BYTE> copy_sid(PSID sid) {
      if (!sid || !IsValidSid(sid)) {
        return {};
      }
      const DWORD len = GetLengthSid(sid);
      std::vector<BYTE> out(len);
      CopySid(len, out.data(), sid);
      return out;
    }

    // ── 主控台使用者 ────────────────────────────────────────────────────

    struct console_user_t {
      bool present = false;
      DWORD console_session = 0xFFFFFFFF;
      DWORD user_rid = 0;
      std::vector<BYTE> logon_sid;  ///< 只拿來比對 pipe 的 ACE，不印
      TOKEN_ELEVATION_TYPE elevation = TokenElevationTypeDefault;
      DWORD il_rid = 0;
    };

    console_user_t get_console_user() {
      console_user_t u;
      u.console_session = WTSGetActiveConsoleSessionId();
      handle_t token {platf::retrieve_users_token(false)};
      if (!token) {
        return u;
      }
      u.present = true;

      DWORD len = 0;
      GetTokenInformation(token.h, TokenUser, nullptr, 0, &len);
      std::vector<BYTE> buf(len ? len : 1);
      if (len && GetTokenInformation(token.h, TokenUser, buf.data(), len, &len)) {
        u.user_rid = sid_rid(reinterpret_cast<TOKEN_USER *>(buf.data())->User.Sid);
      }

      len = 0;
      GetTokenInformation(token.h, TokenGroups, nullptr, 0, &len);
      buf.assign(len ? len : 1, 0);
      if (len && GetTokenInformation(token.h, TokenGroups, buf.data(), len, &len)) {
        const auto *groups = reinterpret_cast<TOKEN_GROUPS *>(buf.data());
        for (DWORD i = 0; i < groups->GroupCount; ++i) {
          if ((groups->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
            u.logon_sid = copy_sid(groups->Groups[i].Sid);
            break;
          }
        }
      }

      TOKEN_ELEVATION_TYPE et {};
      if (GetTokenInformation(token.h, TokenElevationType, &et, sizeof(et), &len)) {
        u.elevation = et;
      }

      len = 0;
      GetTokenInformation(token.h, TokenIntegrityLevel, nullptr, 0, &len);
      buf.assign(len ? len : 1, 0);
      if (len && GetTokenInformation(token.h, TokenIntegrityLevel, buf.data(), len, &len)) {
        u.il_rid = sid_rid(reinterpret_cast<TOKEN_MANDATORY_LABEL *>(buf.data())->Label.Sid);
      }
      return u;
    }

    std::string_view elevation_name(TOKEN_ELEVATION_TYPE t) {
      switch (t) {
        case TokenElevationTypeFull:
          return "full"sv;
        case TokenElevationTypeLimited:
          return "limited"sv;
        default:
          return "default"sv;
      }
    }

    std::string il_name(DWORD rid) {
      switch (rid) {
        case SECURITY_MANDATORY_UNTRUSTED_RID:
          return "UT";
        case SECURITY_MANDATORY_LOW_RID:
          return "LW";
        case SECURITY_MANDATORY_MEDIUM_RID:
          return "ME";
        case SECURITY_MANDATORY_MEDIUM_PLUS_RID:
          return "MP";
        case SECURITY_MANDATORY_HIGH_RID:
          return "HI";
        case SECURITY_MANDATORY_SYSTEM_RID:
          return "SI";
        default:
          return std::format("0x{:x}", rid);
      }
    }

    // ── 安全描述元分析 ──────────────────────────────────────────────────

    enum class who_e {
      sy,
      ba,
      logon_match,  ///< 主控台使用者的 logon SID
      logon_other,  ///< 別的 logon session
      other,
    };

    struct ace_t {
      BYTE type = 0;
      BYTE flags = 0;
      DWORD mask = 0;
      who_e who = who_e::other;
      DWORD rid = 0;
    };

    struct sd_t {
      DWORD err = ERROR_SUCCESS;
      bool owner_present = false;
      bool owner_sy = false;
      DWORD owner_rid = 0;
      bool dacl_present = false;
      bool dacl_null = false;  ///< NULL DACL＝所有人完全控制
      bool dacl_protected = false;
      std::vector<ace_t> aces;
      std::vector<BYTE> dacl_bytes;  ///< SetSecurityInfo 來回用
      bool label_present = false;
      DWORD label_rid = 0;
      DWORD label_policy = 0;
    };

    sd_t read_sd(HANDLE h, const std::vector<BYTE> &console_logon_sid) {
      sd_t out;
      PSID owner = nullptr;
      PACL dacl = nullptr;
      PACL sacl = nullptr;
      PSECURITY_DESCRIPTOR sd = nullptr;
      out.err = GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, &owner, nullptr, &dacl, &sacl, &sd);
      if (out.err != ERROR_SUCCESS) {
        return out;
      }
      auto free_sd = util::fail_guard([sd]() {
        LocalFree(sd);
      });

      out.owner_present = owner != nullptr;
      out.owner_sy = owner && IsWellKnownSid(owner, WinLocalSystemSid);
      out.owner_rid = sid_rid(owner);

      SECURITY_DESCRIPTOR_CONTROL ctrl = 0;
      DWORD rev = 0;
      if (sd && GetSecurityDescriptorControl(sd, &ctrl, &rev)) {
        out.dacl_present = (ctrl & SE_DACL_PRESENT) != 0;
        out.dacl_protected = (ctrl & SE_DACL_PROTECTED) != 0;
      }
      out.dacl_null = out.dacl_present && dacl == nullptr;

      if (dacl) {
        out.dacl_bytes.assign(reinterpret_cast<const BYTE *>(dacl), reinterpret_cast<const BYTE *>(dacl) + dacl->AclSize);
        for (DWORD i = 0; i < dacl->AceCount; ++i) {
          void *p = nullptr;
          if (!GetAce(dacl, i, &p)) {
            continue;
          }
          const auto *hdr = static_cast<const ACE_HEADER *>(p);
          ace_t a;
          a.type = hdr->AceType;
          a.flags = hdr->AceFlags;
          if (hdr->AceType == ACCESS_ALLOWED_ACE_TYPE || hdr->AceType == ACCESS_DENIED_ACE_TYPE) {
            const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(p);
            PSID sid = const_cast<DWORD *>(&ace->SidStart);
            a.mask = ace->Mask;
            a.rid = sid_rid(sid);
            if (IsWellKnownSid(sid, WinLocalSystemSid)) {
              a.who = who_e::sy;
            } else if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) {
              a.who = who_e::ba;
            } else if (is_logon_sid(sid)) {
              a.who = !console_logon_sid.empty() && EqualSid(sid, const_cast<BYTE *>(console_logon_sid.data())) ? who_e::logon_match : who_e::logon_other;
            }
          }
          out.aces.push_back(a);
        }
      }

      if (sacl) {
        for (DWORD i = 0; i < sacl->AceCount; ++i) {
          void *p = nullptr;
          if (!GetAce(sacl, i, &p)) {
            continue;
          }
          const auto *hdr = static_cast<const ACE_HEADER *>(p);
          if (hdr->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
            const auto *ace = static_cast<const SYSTEM_MANDATORY_LABEL_ACE *>(p);
            out.label_present = true;
            out.label_rid = sid_rid(const_cast<DWORD *>(&ace->SidStart));
            out.label_policy = ace->Mask;
          }
        }
      }
      return out;
    }

    std::string policy_name(DWORD policy) {
      std::string s;
      if (policy & SYSTEM_MANDATORY_LABEL_NO_WRITE_UP) {
        s += "NW";
      }
      if (policy & SYSTEM_MANDATORY_LABEL_NO_READ_UP) {
        s += "NR";
      }
      if (policy & SYSTEM_MANDATORY_LABEL_NO_EXECUTE_UP) {
        s += "NX";
      }
      return s.empty() ? "-"s : s;
    }

    std::string describe(const sd_t &sd) {
      if (sd.err != ERROR_SUCCESS) {
        return std::format("sd=error-{}", sd.err);
      }
      std::string s = "owner=";
      s += !sd.owner_present ? "none"s : sd.owner_sy ? "SY"s : std::format("rid-{}", sd.owner_rid);
      s += " dacl=";
      s += !sd.dacl_present ? "absent"sv : sd.dacl_null ? "null"sv : sd.dacl_protected ? "protected"sv : "inherits"sv;
      s += " aces=[";
      bool first = true;
      for (const auto &a : sd.aces) {
        if (!first) {
          s += ',';
        }
        first = false;
        if (a.type == ACCESS_DENIED_ACE_TYPE) {
          s += "deny:";
        } else if (a.type != ACCESS_ALLOWED_ACE_TYPE) {
          s += std::format("type{}:", a.type);
        }
        switch (a.who) {
          case who_e::sy:
            s += "SY";
            break;
          case who_e::ba:
            s += "BA";
            break;
          case who_e::logon_match:
            s += "logon(console)";
            break;
          case who_e::logon_other:
            s += "logon(other)";
            break;
          default:
            s += std::format("rid-{}", a.rid);
            break;
        }
        s += std::format(":0x{:x}", a.mask);
        if (a.flags) {
          s += std::format("/f0x{:x}", a.flags);
        }
      }
      s += "] label=";
      s += sd.label_present ? il_name(sd.label_rid) + ":" + policy_name(sd.label_policy) : "none"s;
      return s;
    }

    struct sd_expect_t {
      DWORD full_mask;  ///< 型別的完整權限（pipe 0x1F01FF、section 0xF001F、event 0x1F0003）
      std::optional<who_e> second_who;  ///< 第二個 ACE 的主體（BA 或主控台 logon SID）
      DWORD second_mask = 0;
      std::optional<DWORD> label_rid;  ///< 要求的 mandatory label
      DWORD label_policy = 0;
    };

    /// @return 空字串＝符合；否則是第一個不符之處
    std::string sd_mismatch(const sd_t &sd, const sd_expect_t &e) {
      if (sd.err != ERROR_SUCCESS) {
        return std::format("GetSecurityInfo err={}", sd.err);
      }
      if (!sd.owner_sy) {
        return "owner is not SYSTEM";
      }
      if (!sd.dacl_present || sd.dacl_null) {
        return "no DACL (everyone has full access)";
      }
      if (!sd.dacl_protected) {
        return "DACL is not protected (inherits)";
      }
      int sy = 0;
      int second = 0;
      for (const auto &a : sd.aces) {
        if (a.type != ACCESS_ALLOWED_ACE_TYPE) {
          return std::format("unexpected ACE type {}", a.type);
        }
        if (a.flags != 0) {
          return std::format("ACE has flags 0x{:x} (inherited or inheritable)", a.flags);
        }
        if (a.who == who_e::sy) {
          if (a.mask != e.full_mask && !(a.mask & GENERIC_ALL)) {
            return std::format("SYSTEM ACE mask 0x{:x} != 0x{:x}", a.mask, e.full_mask);
          }
          ++sy;
        } else if (e.second_who && a.who == *e.second_who) {
          if (a.mask != e.second_mask) {
            return std::format("second ACE mask 0x{:x} != 0x{:x}", a.mask, e.second_mask);
          }
          ++second;
        } else {
          return "unexpected ACE principal";
        }
      }
      if (sy != 1) {
        return std::format("expected exactly one SYSTEM ACE, got {}", sy);
      }
      if (e.second_who && second != 1) {
        return std::format("expected exactly one {} ACE, got {}", *e.second_who == who_e::ba ? "BA"sv : "console logon SID"sv, second);
      }
      if (e.label_rid) {
        if (!sd.label_present) {
          return "no mandatory label";
        }
        if (sd.label_rid != *e.label_rid || (sd.label_policy & 0x7) != e.label_policy) {
          return std::format("label {}:{} != {}:{}", il_name(sd.label_rid), policy_name(sd.label_policy), il_name(*e.label_rid), policy_name(e.label_policy));
        }
      }
      return {};
    }

    /// pipe 的名稱（GetFileInformationByHandleEx 回的是 `\VipleStreamVR-1` 這種不含 `\\.\pipe` 的形式）
    std::wstring pipe_name(HANDLE h) {
      std::vector<BYTE> buf(sizeof(FILE_NAME_INFO) + 512 * sizeof(wchar_t));
      if (!GetFileInformationByHandleEx(h, FileNameInfo, buf.data(), static_cast<DWORD>(buf.size()))) {
        return {};
      }
      const auto *info = reinterpret_cast<const FILE_NAME_INFO *>(buf.data());
      return std::wstring(info->FileName, info->FileNameLength / sizeof(wchar_t));
    }

    // ── 與 vr_bridge（S1-07）的接縫：selftest 只透過這裡碰 bridge ────────────

    namespace bridge_view {
      struct status_t {
        bool connected = false;
        bool ready = false;
        bool peer_is_selftest = false;
        uint64_t generation = 0;
        std::string phase;
        std::string pipe_reason;
        int64_t server_hb = 0;  ///< header.server_heartbeat_qpc 最近一次寫入的值（連線中每 100 ms）
        std::string last_teardown_reason;
        uint64_t trk_published = 0;
        uint64_t trk_dropped = 0;
      };

      status_t status() {
        const auto s = vr::bridge::status();
        return {s.connected, s.ready, s.peer_is_selftest, s.generation, vr::bridge::phase_name(s.phase), s.pipe_reason, s.last_server_heartbeat_qpc, s.last_teardown_reason, s.trk_published, s.trk_dropped};
      }

      using counters_t = vr::bridge::counters_t;

      counters_t counters() {
        return vr::bridge::counters();
      }

      uint64_t rejects(const counters_t &c, unsigned reason) {
        return reason < std::size(c.rejects) ? c.rejects[reason] : 0;
      }

      uint64_t rejects_total(const counters_t &c) {
        uint64_t n = 0;
        for (const auto v : c.rejects) {
          n += v;
        }
        return n;
      }

      uint64_t byes(const counters_t &c, unsigned reason) {
        return reason < std::size(c.byes_sent) ? c.byes_sent[reason] : 0;
      }

      /// bridge 目前的 pipe、shm、event（最小權限複本：只有 READ_CONTROL〔pipe 另加 FILE_READ_ATTRIBUTES〕，這裡負責關；
      /// 只能讀 SD 與名稱——沒有 WRITE_DAC，要改 DACL 只能經 bridge 的 check_pipe_security）
      struct objects_t {
        handle_t pipe;
        handle_t shm;
        handle_t evt_trk;
        handle_t evt_frm;
      };

      objects_t objects() {
        const auto o = vr::bridge::duplicate_objects_for_selftest();
        objects_t out;
        out.pipe = handle_t {static_cast<HANDLE>(o.pipe)};
        out.shm = handle_t {static_cast<HANDLE>(o.shm)};
        out.evt_trk = handle_t {static_cast<HANDLE>(o.evt_trk)};
        out.evt_frm = handle_t {static_cast<HANDLE>(o.evt_frm)};
        return out;
      }

      /// @return bridge 的基本檢查通過（eye、packed、格式、Hz、LUID）
      bool set_config(const vripc_session_config_t &c) {
        return vr::bridge::set_session_config(c);
      }

      void clear_config() {
        vr::bridge::clear_session_config();
      }

      /// @return bridge 接受放行（它會自己複製 handle，並要求映像等於 `<install>\tools\vr_probe\vr_probe.exe`）
      bool allow_peer(HANDLE child, const FILETIME &created) {
        return vr::bridge::allow_selftest_peer(static_cast<void *>(child), created);
      }

      void revoke_peer() {
        vr::bridge::revoke_selftest_peer();
      }

      /// 要求 bridge 立刻重查主控台使用者（平時靠輪詢），讓 pipe 的使用者 ACE 就位
      void refresh_user() {
        vr::bridge::on_user_session_changed();
      }

      /// 在呼叫端執行緒上當 server 端消費者跑 duration（§B.7 的 fence 確認與 §B.8 驗證，V3 display_vr_t 之前的替身）
      vr::bridge::consume_report_t consume(std::chrono::milliseconds duration, const std::atomic<bool> &stop) {
        return vr::bridge::selftest_consume(duration, [&stop]() {
          return stop.load();
        });
      }

      /// T0：讀＋原樣寫回 VR pipe 的 DACL，兩步都在 bridge 的 pipe_mtx_ 內（不與 refresh_user() 換 ACE 競態）
      vr::bridge::pipe_sd_report_t security_roundtrip() {
        return vr::bridge::check_pipe_security(true);
      }

      /// 送一則 STATE（REQUEST_QUIT）到目前的連線；T2.never-read 拿它塞滿不讀 pipe 的 peer 的輸入緩衝。
      /// 對 vr_probe 無作用（peer 不讀，或讀到也只印一行）；T2 要求 SteamVR 沒在跑，不會送到真的 vrserver。
      void send_state_filler() {
        vr::bridge::request_steamvr_quit();
      }

      /// K26 的第三個寫入者（selftest 合成來源）；與 0x5506 handler 走同一個 writer mutex
      void publish(const vr::bridge::tracked_sample_t &s) {
        vr::bridge::publish_tracking(s);
      }

      bool tracking_wanted() {
        return vr::bridge::tracking_wanted();
      }

      constexpr uint32_t identity_selftest_peer = 2;
    }  // namespace bridge_view

    // ── DXGI adapter ────────────────────────────────────────────────────

    struct adapter_info_t {
      UINT index = 0;
      std::string desc;
      UINT vendor = 0;
      SIZE_T vram = 0;
      UINT flags = 0;
      LUID luid {};
    };

    std::vector<adapter_info_t> list_adapters() {
      std::vector<adapter_info_t> out;
      IDXGIFactory1 *factory = nullptr;
      if (FAILED(CreateDXGIFactory1(IID_IDXGIFactory1, reinterpret_cast<void **>(&factory))) || !factory) {
        return out;
      }
      IDXGIAdapter1 *adapter = nullptr;
      for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d {};
        if (SUCCEEDED(adapter->GetDesc1(&d))) {
          adapter_info_t a;
          a.index = i;
          a.desc = utf_utils::to_utf8(d.Description);
          a.vendor = d.VendorId;
          a.vram = d.DedicatedVideoMemory;
          a.flags = d.Flags;
          a.luid = d.AdapterLuid;
          out.push_back(std::move(a));
        }
        adapter->Release();
      }
      factory->Release();
      return out;
    }

    /// selftest 用的 adapter：非軟體 adapter 中 VRAM 最大的一張（production 的 K22 規則在 V5 的 probe_environment）
    std::optional<adapter_info_t> pick_adapter(const std::vector<adapter_info_t> &list) {
      std::optional<adapter_info_t> best;
      for (const auto &a : list) {
        if (a.flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
          continue;
        }
        if (!best || a.vram > best->vram) {
          best = a;
        }
      }
      return best;
    }

    bool process_running(const wchar_t *exe) {
      handle_t snap {CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
      if (!snap) {
        return false;
      }
      PROCESSENTRY32W pe {};
      pe.dwSize = sizeof(pe);
      for (BOOL ok = Process32FirstW(snap.h, &pe); ok; ok = Process32NextW(snap.h, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe) == 0) {
          return true;
        }
      }
      return false;
    }

    // ── vr_probe 子行程 ──────────────────────────────────────────────────

    /**
     * @brief 以主控台使用者身分執行的 vr_probe（`run_command(false, …)`）：放進 kill-on-close 的 job
     *        （server 被殺時一起結束，sec-M13），stdout 經匿名 pipe 回來，逐行以 sanitize_probe_line 過濾。
     */
    class probe_proc_t {
    public:
      enum class wait_e {
        exited,
        timeout,
        stopped,
        cut,  ///< on_tick 回 false：情境提早完成，行程被結束
      };

      probe_proc_t(const std::filesystem::path &exe, const std::string &args) {
        launch(exe, args);
      }

      probe_proc_t(const probe_proc_t &) = delete;
      probe_proc_t &operator=(const probe_proc_t &) = delete;

      ~probe_proc_t() {
        terminate();
        finish_reader();
        if (read_) {
          CloseHandle(read_);
        }
      }

      bool ok() const {
        return ok_;
      }

      const std::string &error() const {
        return error_;
      }

      HANDLE process() const {
        return ok_ ? child_.native_handle() : nullptr;
      }

      FILETIME created() const {
        return created_;
      }

      DWORD pid() const {
        return pid_;
      }

      /// 等行程結束；on_tick 每 20 ms 呼叫一次，回 false 表示情境已完成（結束行程）
      wait_e wait(std::chrono::milliseconds limit, const std::atomic<bool> &stop, const std::function<bool()> &on_tick = {}) {
        if (!ok_) {
          return wait_e::exited;
        }
        const auto t0 = steady::now();
        wait_e result;
        for (;;) {
          if (WaitForSingleObject(child_.native_handle(), 20) == WAIT_OBJECT_0) {
            result = wait_e::exited;
            break;
          }
          if (stop.load()) {
            result = wait_e::stopped;
            break;
          }
          if (on_tick && !on_tick()) {
            result = wait_e::cut;
            break;
          }
          if (steady::now() - t0 >= limit) {
            result = wait_e::timeout;
            break;
          }
        }
        if (result != wait_e::exited) {
          terminate();
        }
        finish_reader();
        return result;
      }

      /// 結束整個 job（vr_probe 與它可能開的任何東西）
      void terminate() {
        if (!group_) {
          return;
        }
        if (ok_ && WaitForSingleObject(child_.native_handle(), 0) == WAIT_TIMEOUT) {
          TerminateJobObject(group_->native_handle(), 1);
          WaitForSingleObject(child_.native_handle(), 2000);
        }
      }

      std::optional<DWORD> exit_code() const {
        DWORD code = 0;
        if (!ok_ || !GetExitCodeProcess(child_.native_handle(), &code) || code == STILL_ACTIVE) {
          return std::nullopt;
        }
        return code;
      }

      std::vector<std::string> lines() const {
        std::lock_guard lk {m_};
        return lines_;
      }

      /// 最後一行 tag（與 sub）相符的 probe 行
      std::optional<probe_kv_t> last(std::string_view tag, std::string_view sub = {}) const {
        std::lock_guard lk {m_};
        for (auto it = lines_.rbegin(); it != lines_.rend(); ++it) {
          auto kv = parse_probe_kv(*it);
          if (kv.tag == tag && (sub.empty() || kv.sub == sub)) {
            return kv;
          }
        }
        return std::nullopt;
      }

      std::vector<probe_kv_t> all(std::string_view tag, std::string_view sub = {}) const {
        std::vector<probe_kv_t> out;
        std::lock_guard lk {m_};
        for (const auto &l : lines_) {
          auto kv = parse_probe_kv(l);
          if (kv.tag == tag && (sub.empty() || kv.sub == sub)) {
            out.push_back(std::move(kv));
          }
        }
        return out;
      }

      /// 最後一行（診斷用，已過濾）
      std::string last_line() const {
        std::lock_guard lk {m_};
        return lines_.empty() ? "<none>"s : lines_.back();
      }

    private:
      void launch(const std::filesystem::path &exe, const std::string &args) {
        if (auto why = probe_unusable(exe)) {
          error_ = *why;
          return;
        }

        try {
          group_ = std::make_unique<bp::group>();
        } catch (const std::exception &e) {
          error_ = std::format("job creation failed: {}", e.what());
          return;
        }
        // kill-on-close：server 行程被殺時 kernel 關掉 job handle，vr_probe 一起結束（不留孤兒）。
        // silent breakaway（M1b V4 實測）：vr_probe 的 VR_Init 可能讓 SteamVR 重新拉起 vrmonitor 等行程，它們若留在 job 裡，
        // job 關閉時會被一起殺掉（vrserver 看到「Lost master process」就整個 SteamVR 退出）。只有 vr_probe 本身留在 job 內。
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info {};
        if (!QueryInformationJobObject(group_->native_handle(), JobObjectExtendedLimitInformation, &info, sizeof(info), nullptr)) {
          error_ = std::format("QueryInformationJobObject err={}", GetLastError());
          return;
        }
        info.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
        if (!SetInformationJobObject(group_->native_handle(), JobObjectExtendedLimitInformation, &info, sizeof(info))) {
          error_ = std::format("SetInformationJobObject(kill-on-close) err={}", GetLastError());
          return;
        }

        // stdout：可繼承的寫端交給 run_command（HANDLE_LIST 只放這一個），讀端不可繼承
        SECURITY_ATTRIBUTES sa {sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr;
        HANDLE wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 65536)) {
          error_ = std::format("CreatePipe err={}", GetLastError());
          return;
        }
        read_ = rd;
        SetHandleInformation(read_, HANDLE_FLAG_INHERIT, 0);
        const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(wr), _O_WRONLY | _O_BINARY);
        if (fd == -1) {
          CloseHandle(wr);
          error_ = "_open_osfhandle failed";
          return;
        }
        FILE *file = _fdopen(fd, "wb");
        if (!file) {
          _close(fd);
          error_ = "_fdopen failed";
          return;
        }

        const std::string cmd = "\"" + utf_utils::to_utf8(exe.wstring()) + "\" " + args;
        boost::filesystem::path working_dir {exe.parent_path().wstring()};
        bp::environment env;  // 空：SYSTEM 以使用者身分啟動時，run_command 以使用者自己的環境區塊為準
        std::error_code ec;
        child_ = platf::run_command(false, false, cmd, working_dir, env, file, ec, group_.get());
        fclose(file);  // 關掉我們這份寫端；子行程持有繼承來的那份，它結束時讀端才會收到 EOF

        reader_ = std::thread([this]() {
          reader_main();
        });

        if (ec || !child_.valid()) {
          error_ = std::format("launch failed: {} (is a console user logged in?)", ec ? ec.message() : "no process"s);
          return;
        }

        HANDLE h = child_.native_handle();
        BOOL in_job = FALSE;
        if (!IsProcessInJob(h, group_->native_handle(), &in_job) || !in_job) {
          // run_command 以 PID 重新開啟行程；確認開到的就是 job 裡的那一個
          TerminateJobObject(group_->native_handle(), 1);
          error_ = "launched process is not in the selftest job";
          return;
        }
        FILETIME exit_time {};
        FILETIME kernel {};
        FILETIME user {};
        if (!GetProcessTimes(h, &created_, &exit_time, &kernel, &user)) {
          TerminateJobObject(group_->native_handle(), 1);
          error_ = std::format("GetProcessTimes err={}", GetLastError());
          return;
        }
        pid_ = GetProcessId(h);
        ok_ = true;
      }

      void reader_main() {
        std::string acc;
        std::array<char, 4096> buf {};
        for (;;) {
          DWORD avail = 0;
          if (!PeekNamedPipe(read_, nullptr, 0, nullptr, &avail, nullptr)) {
            break;  // 寫端全部關閉（ERROR_BROKEN_PIPE）
          }
          if (avail == 0) {
            if (reader_stop_.load()) {
              break;
            }
            std::this_thread::sleep_for(10ms);
            continue;
          }
          DWORD n = 0;
          if (!ReadFile(read_, buf.data(), std::min<DWORD>(avail, static_cast<DWORD>(buf.size())), &n, nullptr) || n == 0) {
            break;
          }
          acc.append(buf.data(), n);
          std::size_t pos;
          while ((pos = acc.find('\n')) != std::string::npos) {
            handle_line(std::string_view {acc}.substr(0, pos));
            acc.erase(0, pos + 1);
          }
          if (acc.size() > 65536) {
            acc.clear();  // 沒有換行的超長輸出：丟掉
          }
        }
        if (!acc.empty()) {
          handle_line(acc);
        }
        reader_done_.store(true);
      }

      void handle_line(std::string_view raw) {
        auto s = sanitize_probe_line(raw);
        if (!s) {
          return;
        }
        std::lock_guard lk {m_};
        if (lines_.size() < 4000) {
          lines_.push_back(std::move(*s));
        }
      }

      void finish_reader() {
        if (!reader_.joinable()) {
          return;
        }
        // 行程結束後 pipe 會被排空、PeekNamedPipe 以 BROKEN_PIPE 結束；2 s 內沒結束（例如有孫行程繼承了寫端）就強制停
        const auto t0 = steady::now();
        while (!reader_done_.load() && steady::now() - t0 < 2s) {
          std::this_thread::sleep_for(5ms);
        }
        reader_stop_.store(true);
        reader_.join();
      }

      std::unique_ptr<bp::group> group_;
      bp::child child_;
      HANDLE read_ = nullptr;
      std::thread reader_;
      std::atomic<bool> reader_stop_ {false};
      std::atomic<bool> reader_done_ {false};
      mutable std::mutex m_;
      std::vector<std::string> lines_;
      bool ok_ = false;
      std::string error_;
      FILETIME created_ {};
      DWORD pid_ = 0;
    };

    // ── T0 ──────────────────────────────────────────────────────────────

    void check_sd(reporter_t &r, std::string_view id, HANDLE h, const console_user_t &user, const sd_expect_t &e) {
      const auto sd = read_sd(h, user.logon_sid);
      const auto why = sd_mismatch(sd, e);
      r.check(id, why.empty(), why.empty() ? describe(sd) : describe(sd) + " mismatch=\"" + why + "\"");
    }

    /**
     * @brief 不具名 shm／event 的 SD（§B.1：O:SYG:SYD:P(A;;GA;;;SY)）。
     * @details 型別不要求安全性的不具名物件，物件管理員可能根本不保存 SD（U30／sec-m9，UNVERIFIED）；
     *          那種情況只記 INFO（最小權限複製是衛生措施，不是信任邊界），有 SD 但內容不符才算 FAIL。
     */
    void check_unnamed_sd(reporter_t &r, std::string_view id, HANDLE h, const console_user_t &user, DWORD full_mask) {
      if (!h) {
        r.check(id, false, "object=missing (bridge returned no handle)"sv);
        return;
      }
      const auto sd = read_sd(h, user.logon_sid);
      if (sd.err == ERROR_NO_SECURITY_ON_OBJECT || (sd.err == ERROR_SUCCESS && !sd.owner_present && !sd.dacl_present)) {
        r.info(id, std::format("sd=none err={} (U30: unnamed object keeps no security descriptor; duplicated-handle rights are hygiene only)", sd.err));
        return;
      }
      const auto why = sd_mismatch(sd, {full_mask, std::nullopt, 0, std::nullopt, 0});
      r.check(id, why.empty(), why.empty() ? describe(sd) : describe(sd) + " mismatch=\"" + why + "\"");
    }

    /// 本 generation 的 shm 與兩個 event（只在有連線的 driver／peer 時存在）
    bool check_generation_objects(reporter_t &r, std::string_view prefix, const console_user_t &user) {
      auto objs = bridge_view::objects();
      if (!objs.shm && !objs.evt_trk && !objs.evt_frm) {
        return false;
      }
      check_unnamed_sd(r, std::string {prefix} + "-shm", objs.shm.h, user, SECTION_ALL_ACCESS);
      check_unnamed_sd(r, std::string {prefix} + "-evt-trk", objs.evt_trk.h, user, EVENT_ALL_ACCESS);
      check_unnamed_sd(r, std::string {prefix} + "-evt-frm", objs.evt_frm.h, user, EVENT_ALL_ACCESS);
      return true;
    }

    // ── T2 ──────────────────────────────────────────────────────────────

    struct t2_ctx_t {
      reporter_t &r;
      const std::atomic<bool> &stop;
      std::filesystem::path probe;
      const console_user_t &user;
      vripc_session_config_t cfg {};
    };

    vripc_session_config_t make_config(const adapter_info_t &a, uint16_t space_epoch, uint8_t layout_epoch) {
      vripc_session_config_t c {};
      c.eye_width = 1024;
      c.eye_height = 1024;
      c.packed_width = 2048;
      c.packed_height = 1024;
      c.dxgi_format = VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM;
      c.refresh_mhz = 90000;
      c.period_ns = 11'111'111;
      for (auto &eye : c.fov_tan) {
        eye[0] = -1.0f;  // left
        eye[1] = 1.0f;  // right
        eye[2] = 1.0f;  // up
        eye[3] = -1.0f;  // down
      }
      const float half_ipd = 0.0315f;
      const float eth[2][7] = {{-half_ipd, 0, 0, 0, 0, 0, 1}, {half_ipd, 0, 0, 0, 0, 0, 1}};
      for (int e = 0; e < 2; ++e) {
        std::copy(std::begin(eth[e]), std::end(eth[e]), std::begin(c.eye_to_head[e]));
      }
      c.ipd_m = 2 * half_ipd;
      c.overscan_deg = 0;
      c.controller_profile = 0;
      c.vr_caps = 0;
      c.universe_id = 0x5649504Cu;  // K24
      c.adapter_luid_low = a.luid.LowPart;
      c.adapter_luid_high = a.luid.HighPart;
      c.space_epoch = space_epoch;
      c.layout_epoch = layout_epoch;
      c.dev_arg = 0;
      c.dev_flags = 0;
      c.vsync_to_photons_us = 11111;
      c.hmd_extrap_cap_us = 50000;
      c.ctrl_extrap_cap_us = 50000;
      c.stale_oor_ctrl_us = 100000;
      c.stale_zero_vel_us = 22222;
      c.stale_oor_hmd_us = 1000000;
      return c;
    }

    /// 情境開始前：上一個 peer 的 teardown 要做完（bridge 回到 LISTENING）
    bool wait_idle(t2_ctx_t &c, std::string_view id) {
      if (wait_until(3000ms, c.stop, []() {
            return !bridge_view::status().connected;
          }) < 0) {
        if (!c.stop.load()) {
          c.r.check(id, false, "precondition: bridge still connected from the previous scenario"sv);
        }
        return false;
      }
      return true;
    }

    /// 約 1 ms 的等待（高解析度 waitable timer；建不出來時退回 sleep_for，粒度可能變成 15.6 ms）
    class hr_tick_t {
    public:
      hr_tick_t() {
        timer_ = handle_t {CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
        if (!timer_) {
          timer_ = handle_t {CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS)};
        }
      }

      void sleep_1ms() {
        LARGE_INTEGER due;
        due.QuadPart = -10000;  // 1 ms（100 ns 單位，相對）
        if (timer_ && SetWaitableTimer(timer_.h, &due, 0, nullptr, nullptr, FALSE)) {
          WaitForSingleObject(timer_.h, 100);
        } else {
          std::this_thread::sleep_for(1ms);
        }
      }

    private:
      handle_t timer_;
    };

    std::string peer_diag(const probe_proc_t &p) {
      const auto code = p.exit_code();
      return std::format("peerExit={} peerLast=\"{}\"", code ? std::to_string(*code) : "running"s, p.last_line());
    }

    /// 把 vr_probe 的輸出（已過濾）轉給 CLI；只送 sink、不進 sunshine.log，前綴由 server 重新加上（sec-m14）
    void relay(reporter_t &r, std::string_view id, const probe_proc_t &p) {
      const std::string tag = std::string {id} + ".probe";
      for (const auto &l : p.lines()) {
        if (std::string_view {l}.starts_with("abi-row "sv)) {
          continue;  // ABI 表另外逐列比對（T6.abi-table），不重複轉印
        }
        r.verbose(tag, l);
      }
    }

    /// 啟動一個 ipcpeer；allow 時登記成 selftest peer（bridge 自己複製 handle、驗映像）。失敗時已回報 FAIL 並回 nullptr。
    std::unique_ptr<probe_proc_t> launch_peer(t2_ctx_t &c, std::string_view id, const std::string &args, bool allow) {
      auto p = std::make_unique<probe_proc_t>(c.probe, args);
      if (!p->ok()) {
        c.r.check(id, false, "launch-failed " + p->error());
        return nullptr;
      }
      if (allow && !bridge_view::allow_peer(p->process(), p->created())) {
        c.r.check(id, false, "bridge refused allow_selftest_peer (the image must be <install>\\tools\\vr_probe\\vr_probe.exe)"sv);
        return nullptr;
      }
      return p;
    }

    /**
     * @brief T2.handshake：正常握手（HELLO→WELCOME→TEXTURES→READY）＋本 generation 物件的 SD＋正常結束的 teardown。
     * vr_probe：`--mode ipcpeer --seconds 3`
     *
     * 放行（allow_selftest_peer）在 run_command 回來後 1 ms 內完成，比 vr_probe 的行程初始化與 HELLO 早得多；
     * 萬一真的輸給競態，第一次連線會被 REJECT(IDENTITY)、ipc_client 退避後重連成功：這種「恰好 1 次 IDENTITY」
     * 容許並記 raceReject=1（冒名情境另外驗）。
     */
    void t2_handshake(t2_ctx_t &c) {
      constexpr auto id = "T2.handshake"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      const auto c0 = bridge_view::counters();
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 3", true);
      if (!p) {
        return;
      }
      const auto t0 = steady::now();
      int64_t ready_ms = -1;
      uint64_t gen = 0;
      bool flagged_selftest = false;
      bool sd_done = false;
      p->wait(12s, c.stop, [&]() {
        const auto s = bridge_view::status();
        if (ready_ms < 0 && s.connected && s.ready) {
          ready_ms = ms_since(t0);
          gen = s.generation;
          flagged_selftest = s.peer_is_selftest;
        }
        if (ready_ms >= 0 && !sd_done) {
          sd_done = check_generation_objects(c.r, "T2.sd"sv, c.user);
        }
        return true;
      });
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      const int64_t teardown_ms = wait_until(1000ms, c.stop, []() {
        return !bridge_view::status().connected;
      });
      const auto c1 = bridge_view::counters();

      const uint64_t d_hs = delta(c1.handshakes_ok, c0.handshakes_ok);
      const uint64_t d_rej = delta(bridge_view::rejects_total(c1), bridge_view::rejects_total(c0));
      const uint64_t d_rej_id = delta(bridge_view::rejects(c1, VRIPC_REJ_IDENTITY), bridge_view::rejects(c0, VRIPC_REJ_IDENTITY));
      const bool race_reject = d_rej == 1 && d_rej_id == 1;
      const bool pass = ready_ms >= 0 && d_hs >= 1 && (d_rej == 0 || race_reject) && c1.last_identity == bridge_view::identity_selftest_peer && flagged_selftest;
      c.r.check(id, pass, std::format("gen={} readyMs={} handshakes=+{} rejects=+{} raceReject={} identity={} {}", gen, ready_ms, d_hs, d_rej, race_reject ? 1 : 0, c1.last_identity == bridge_view::identity_selftest_peer ? "selftest-peer"sv : "other"sv, peer_diag(*p)));
      relay(c.r, id, *p);
      if (!sd_done) {
        c.r.check("T2.sd"sv, false, "shm/event handles were not available while the peer was connected"sv);
      }
      c.r.check("T2.teardown"sv, teardown_ms >= 0 && delta(c1.teardowns, c0.teardowns) >= 1, std::format("afterExitMs={} teardowns=+{}", teardown_ms, delta(c1.teardowns, c0.teardowns)));

      // 最小權限複製（衛生措施，§B.1）：peer 若回報收到的 handle 權限（NtQueryObject 的 GrantedAccess）就核對
      const auto summary = p->last("ipcpeer"sv, "summary"sv);
      if (summary && summary->get("dup_shm")) {
        const auto shm = summary->get_int("dup_shm").value_or(-1);
        const auto trk = summary->get_int("dup_trk").value_or(-1);
        const auto frm = summary->get_int("dup_frm").value_or(-1);
        const bool ok = shm == (SECTION_MAP_READ | SECTION_MAP_WRITE) && trk == SYNCHRONIZE && frm == EVENT_MODIFY_STATE;
        c.r.check("T2.dup-access"sv, ok, std::format("shm=0x{:x} trk=0x{:x} frm=0x{:x} (expect 0x6/0x100000/0x2)", shm, trk, frm));
      } else {
        c.r.info("T2.dup-access"sv, "not reported by the peer"sv);
      }
    }

    /**
     * @brief T2.consume（E-3；§B.7／§B.8 的執行期部分）：peer READY 後以 bridge 的 selftest 消費者跑 3 s：
     *        CPU 端確認 fence、CopyResource、Signal(consumedFence)，每一筆 descriptor 都經 §B.8 驗證。
     *        正常 peer 應該：有幀、0 不合格、0 fence 逾時／遺失。Signal→CPU 可見延遲與複製時間只記錄（INFO）。
     * vr_probe：`--mode ipcpeer --seconds 8`
     */
    void t2_consume(t2_ctx_t &c) {
      constexpr auto id = "T2.consume"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 8", true);
      if (!p) {
        return;
      }
      std::optional<vr::bridge::consume_report_t> rep;
      p->wait(12s, c.stop, [&]() {
        const auto s = bridge_view::status();
        if (s.connected && s.ready) {
          rep = bridge_view::consume(3000ms, c.stop);
          return false;
        }
        return true;
      });
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      if (!rep) {
        c.r.check(id, false, "peer never became READY " + peer_diag(*p));
        relay(c.r, id, *p);
        return;
      }
      const auto &x = *rep;
      // fence 逾時（§B.7 的 2 ms CPU 等待）是 GPU 排程的偶發延遲，不是協定錯誤：比照 §F.5 T2 live 的「消費率 ≥ 99%」，
      // 容許 ≤ 1%（至少 1 筆）；不合格、fence 遺失一律 0。
      const uint64_t timeout_budget = std::max<uint64_t>(1, x.frames_copied / 100);
      const bool pass = x.ok && x.frames_copied > 0 && x.frames_invalid == 0 && x.frames_fence_timeout <= timeout_budget && x.frames_fence_lost == 0;
      c.r.check(id, pass, std::format("ok={} gens={} copied={} skipped={} invalid={} torn={} stale={} fenceTimeout={} (budget {}) fenceLost={} wakes={} evtTimeouts={} error=\"{}\" {}", x.ok ? 1 : 0, x.generations_opened, x.frames_copied, x.frames_skipped, x.frames_invalid, x.frames_torn, x.frames_stale, x.frames_fence_timeout, timeout_budget, x.frames_fence_lost, x.evt_wakes, x.evt_timeouts, x.error, peer_diag(*p)));
      c.r.info("T2.consume-latency"sv, std::format("submitToVisibleMs p50={:.3f} p95={:.3f} max={:.3f} copyMs p50={:.3f} p95={:.3f} max={:.3f}", x.submit_to_visible_p50_ms, x.submit_to_visible_p95_ms, x.submit_to_visible_max_ms, x.copy_p50_ms, x.copy_p95_ms, x.copy_max_ms));
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.kill-teardown（E-4）：peer READY 之後被 TerminateJobObject，server 要在 100 ms 內 teardown（pipe 斷線，不等 heartbeat）。
     * vr_probe：`--mode ipcpeer --seconds 30`
     */
    void t2_kill(t2_ctx_t &c) {
      constexpr auto id = "T2.kill-teardown"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      const auto c0 = bridge_view::counters();
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 30", true);
      if (!p) {
        return;
      }
      std::optional<steady::time_point> ready_at;
      int64_t idle_ms = -1;
      const auto w = p->wait(12s, c.stop, [&]() {
        const auto s = bridge_view::status();
        if (!ready_at && s.connected && s.ready) {
          ready_at = steady::now();
        }
        // READY 之後讓它跑 500 ms 再殺；從 TerminateJobObject 前一刻開始計時，1 ms 輪詢
        if (ready_at && steady::now() - *ready_at >= 500ms) {
          const auto t_kill = steady::now();
          p->terminate();
          if (wait_until(2000ms, c.stop, []() {
                return !bridge_view::status().connected;
              },
                         1ms) >= 0) {
            idle_ms = ms_since(t_kill);
          }
          return false;
        }
        return true;
      });
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      const auto c1 = bridge_view::counters();
      const bool pass = ready_at && w == probe_proc_t::wait_e::cut && idle_ms >= 0 && idle_ms <= 100 && delta(c1.teardowns, c0.teardowns) >= 1;
      c.r.check(id, pass, std::format("ready={} killedToIdleMs={} (limit 100) teardowns=+{} {}", ready_at ? 1 : 0, idle_ms, delta(c1.teardowns, c0.teardowns), peer_diag(*p)));
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.reconnect：READY 後 server 換 session 設定 → BYE(RECONFIG) → peer 立刻重連 → generation +1、再次 READY（≤ 3 s）。
     * vr_probe：`--mode ipcpeer --seconds 10`
     */
    void t2_reconnect(t2_ctx_t &c) {
      constexpr auto id = "T2.reconnect"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      const auto c0 = bridge_view::counters();
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 10", true);
      if (!p) {
        return;
      }
      auto cfg2 = c.cfg;
      cfg2.space_epoch = static_cast<uint16_t>(c.cfg.space_epoch + 1);
      cfg2.layout_epoch = static_cast<uint8_t>(c.cfg.layout_epoch + 1);

      uint64_t g1 = 0;
      uint64_t g2 = 0;
      bool cfg2_ok = true;
      std::optional<steady::time_point> reconf_at;
      int64_t reconnect_ms = -1;
      p->wait(14s, c.stop, [&]() {
        const auto s = bridge_view::status();
        if (!reconf_at) {
          if (s.connected && s.ready) {
            g1 = s.generation;
            cfg2_ok = bridge_view::set_config(cfg2);
            reconf_at = steady::now();
          }
          return true;
        }
        if (s.connected && s.ready && s.generation > g1) {
          g2 = s.generation;
          reconnect_ms = ms_since(*reconf_at);
          return false;
        }
        return steady::now() - *reconf_at < 3s;
      });
      bridge_view::revoke_peer();
      // peer 已結束：把設定換回原值（沒有連線時只是存起來，給下一個 generation）
      wait_until(1000ms, c.stop, []() {
        return !bridge_view::status().connected;
      });
      bridge_view::set_config(c.cfg);
      if (c.stop.load()) {
        return;
      }
      const auto c1 = bridge_view::counters();
      const uint64_t d_bye = delta(bridge_view::byes(c1, VRIPC_BYE_RECONFIG), bridge_view::byes(c0, VRIPC_BYE_RECONFIG));
      const uint64_t d_hs = delta(c1.handshakes_ok, c0.handshakes_ok);
      const bool pass = reconf_at && cfg2_ok && g2 > g1 && reconnect_ms >= 0 && reconnect_ms <= 3000 && d_bye >= 1 && d_hs >= 2;
      c.r.check(id, pass, std::format("gen={}->{} reconnectMs={} (limit 3000) configAccepted={} byeReconfig=+{} handshakes=+{} {}", g1, g2, reconnect_ms, cfg2_ok ? 1 : 0, d_bye, d_hs, peer_diag(*p)));
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.fence-max（sec-M12、§B.5–§B.7；對方寫的不合格值在執行期的處理）：peer 在第 30 幀之後把 sharedFence
     *        Signal 成 UINT64_MAX。server 端消費者讀到 UINT64_MAX、確認自己的 device 沒被移除 → BYE(DEVICE_LOST) →
     *        peer 立即重連 → 1 s 內出現新 generation 並 READY。消費者在另一條執行緒跑，這裡以 20 ms 輪詢量時間。
     * vr_probe：`--mode ipcpeer --seconds 12 --peer-fence-max --peer-fence-max-after 30`
     */
    void t2_fence_max(t2_ctx_t &c) {
      constexpr auto id = "T2.fence-max"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      const auto c0 = bridge_view::counters();
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 12 --peer-fence-max --peer-fence-max-after 30", true);
      if (!p) {
        return;
      }
      std::atomic<bool> consume_stop {false};
      std::optional<vr::bridge::consume_report_t> rep;
      std::thread consumer;
      uint64_t g1 = 0;
      uint64_t g2 = 0;
      std::optional<steady::time_point> ready_at;
      std::optional<steady::time_point> lost_at;
      int64_t regen_ms = -1;
      p->wait(16s, c.stop, [&]() {
        const auto s = bridge_view::status();
        if (!ready_at) {
          if (s.connected && s.ready) {
            ready_at = steady::now();
            g1 = s.generation;
            // READY 立刻開始消費：沒有消費者時 peer 三個 slot 用完就發不出第 30 幀
            consumer = std::thread([&]() {
              rep = bridge_view::consume(8000ms, consume_stop);
            });
          }
          return true;
        }
        if (!lost_at && delta(bridge_view::byes(bridge_view::counters(), VRIPC_BYE_DEVICE_LOST), bridge_view::byes(c0, VRIPC_BYE_DEVICE_LOST)) >= 1) {
          lost_at = steady::now();
        }
        if (lost_at && s.connected && s.ready && s.generation > g1) {
          g2 = s.generation;
          regen_ms = ms_since(*lost_at);
          return false;
        }
        return steady::now() - *ready_at < 8s;
      });
      consume_stop.store(true);
      if (consumer.joinable()) {
        consumer.join();
      }
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      const auto c1 = bridge_view::counters();
      const uint64_t d_bye = delta(bridge_view::byes(c1, VRIPC_BYE_DEVICE_LOST), bridge_view::byes(c0, VRIPC_BYE_DEVICE_LOST));
      const uint64_t lost = rep ? rep->frames_fence_lost : 0;
      const bool pass = ready_at && lost_at && g2 > g1 && regen_ms >= 0 && regen_ms <= 1000 && d_bye >= 1;
      c.r.check(id, pass, std::format("gen={}->{} lostToNewGenMs={} (limit 1000) byeDeviceLost=+{} consumerFenceLost={} consumerTeardownRequests={} {}", g1, g2, regen_ms, d_bye, lost, rep ? rep->teardown_requests : 0, peer_diag(*p)));
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.consume-noflush（E-3 對照、U7：Signal 不 Flush 是否可見）：同 T2.consume，但 peer 在 Signal 之後不 Flush。
     *        只記錄（INFO）：沒有 Flush 時 Signal 可能要等到下一次隱含的提交才被 server CPU 看到，逾時多是預期中的
     *        （連續逾時 ≥ 1 s 時消費者會要求 BYE(PROTOCOL_ERROR)，peer 重連）。peer 沒有 READY 才算 FAIL（情境沒跑起來）。
     * vr_probe：`--mode ipcpeer --seconds 8 --peer-no-flush`
     */
    void t2_consume_noflush(t2_ctx_t &c) {
      constexpr auto id = "T2.consume-noflush"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 8 --peer-no-flush", true);
      if (!p) {
        return;
      }
      std::optional<vr::bridge::consume_report_t> rep;
      p->wait(12s, c.stop, [&]() {
        const auto s = bridge_view::status();
        if (s.connected && s.ready) {
          rep = bridge_view::consume(3000ms, c.stop);
          return false;
        }
        return true;
      });
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      if (!rep) {
        c.r.check(id, false, "peer never became READY " + peer_diag(*p));
        relay(c.r, id, *p);
        return;
      }
      const auto &x = *rep;
      c.r.info(id, std::format("copied={} fenceTimeout={} teardownRequests={} invalid={} submitToVisibleMs p50={:.3f} p95={:.3f} max={:.3f} (Signal without Flush; compare with T2.consume-latency)", x.frames_copied, x.frames_fence_timeout, x.teardown_requests, x.frames_invalid, x.submit_to_visible_p50_ms, x.submit_to_visible_p95_ms, x.submit_to_visible_max_ms));
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.stop-signal（E-4、§B.7）：peer 在第 30 幀之後照常發布 descriptor，但不再 Signal sharedFence。
     *        selftest 消費者對每一筆只等 2 ms（CPU、GetCompletedValue＋SetEventOnCompletion），逾時就不複製、照樣放回 slot；
     *        連續逾時累計 ≥ 1 s → request_teardown(PROTOCOL_ERROR) → bridge 送 BYE(PROTOCOL_ERROR)。
     *        判定：有逾時計數、要求拆除當下連續逾時已持續 1000–1500 ms、server 送出 BYE(PROTOCOL_ERROR)。
     *        逾時當下 GetCompletedValue 讀到的值另記一行（U7：預期停在最後一次 Signal 的值，而不是 UINT64_MAX）。
     * vr_probe：`--mode ipcpeer --seconds 10 --peer-stop-signal-after 30`
     */
    void t2_stop_signal(t2_ctx_t &c) {
      constexpr auto id = "T2.stop-signal"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      const auto c0 = bridge_view::counters();
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 10 --peer-stop-signal-after 30", true);
      if (!p) {
        return;
      }
      std::atomic<bool> consume_stop {false};
      std::optional<vr::bridge::consume_report_t> rep;
      std::thread consumer;
      std::optional<steady::time_point> ready_at;
      int64_t ready_to_bye_ms = -1;
      p->wait(14s, c.stop, [&]() {
        if (!ready_at) {
          const auto s = bridge_view::status();
          if (s.connected && s.ready) {
            ready_at = steady::now();
            // READY 立刻開始消費：前 30 幀正常確認，之後每一筆都逾時
            consumer = std::thread([&]() {
              rep = bridge_view::consume(6000ms, consume_stop);
            });
          }
          return true;
        }
        if (delta(bridge_view::byes(bridge_view::counters(), VRIPC_BYE_PROTOCOL_ERROR), bridge_view::byes(c0, VRIPC_BYE_PROTOCOL_ERROR)) >= 1) {
          ready_to_bye_ms = ms_since(*ready_at);
          return false;
        }
        return steady::now() - *ready_at < 6s;
      });
      consume_stop.store(true);
      if (consumer.joinable()) {
        consumer.join();
      }
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      const auto c1 = bridge_view::counters();
      const uint64_t d_bye = delta(bridge_view::byes(c1, VRIPC_BYE_PROTOCOL_ERROR), bridge_view::byes(c0, VRIPC_BYE_PROTOCOL_ERROR));
      const bool timing_ok = rep && rep->fence_timeout_teardown_ms >= 1000.0 && rep->fence_timeout_teardown_ms <= 1500.0;
      const bool pass = ready_at && rep && rep->ok && rep->frames_fence_timeout > 0 && rep->teardown_requests >= 1 && timing_ok && d_bye >= 1;
      c.r.check(id, pass, std::format("fenceTimeouts={} timeoutRunToTeardownMs={:.1f} (window 1000-1500) teardownRequests={} byeProtocolError=+{} readyToByeMs={} copied={} fenceLost={} {}", rep ? rep->frames_fence_timeout : 0, rep ? rep->fence_timeout_teardown_ms : 0.0, rep ? rep->teardown_requests : 0, d_bye, ready_to_bye_ms, rep ? rep->frames_copied : 0, rep ? rep->frames_fence_lost : 0, peer_diag(*p)));
      if (rep && rep->frames_fence_timeout > 0) {
        c.r.info("T2.stop-signal-completed"sv, std::format("GetCompletedValue at the first timeout={} descriptorFence={} (U7: expected to stay at the last signalled value; UINT64_MAX would mean device removed)", rep->fence_timeout_first_completed, rep->fence_timeout_first_expected));
      }
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.ownership（sec-M1、§B.1「送達即移交」）：peer 收到 WELCOME 後關掉兩個 event，立刻開新物件佔位
     *        （handle 表通常重用剛釋放的值）；READY 之後 server 換 session 設定 → BYE(RECONFIG) → peer 重連、新 generation
     *        再做一次。server 在任何路徑都不可從遠端關已送達的 handle（DUPLICATE_CLOSE_SOURCE 只用在寫入沒完整送達時）：
     *        peer 結束前逐一檢查佔位物件仍有效、仍是 signaled，印 `close-after-welcome-check occupants=N intact=M`。
     *        判定：新 generation、BYE(RECONFIG)、N > 0 且 M == N。
     * vr_probe：`--mode ipcpeer --seconds 8 --peer-close-after-welcome`
     */
    void t2_ownership(t2_ctx_t &c) {
      constexpr auto id = "T2.ownership"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      const auto c0 = bridge_view::counters();
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 8 --peer-close-after-welcome", true);
      if (!p) {
        return;
      }
      auto cfg2 = c.cfg;
      cfg2.space_epoch = static_cast<uint16_t>(c.cfg.space_epoch + 1);
      cfg2.layout_epoch = static_cast<uint8_t>(c.cfg.layout_epoch + 1);
      uint64_t g1 = 0;
      uint64_t g2 = 0;
      bool cfg2_ok = true;
      bool reconfigured = false;
      // peer 自己結束時才印檢查行，所以一路等到它結束（不提早切）
      const auto w = p->wait(16s, c.stop, [&]() {
        const auto s = bridge_view::status();
        if (!reconfigured) {
          if (s.connected && s.ready) {
            g1 = s.generation;
            cfg2_ok = bridge_view::set_config(cfg2);
            reconfigured = true;
          }
        } else if (g2 == 0 && s.connected && s.ready && s.generation > g1) {
          g2 = s.generation;
        }
        return true;
      });
      bridge_view::revoke_peer();
      wait_until(1000ms, c.stop, []() {
        return !bridge_view::status().connected;
      });
      bridge_view::set_config(c.cfg);  // 換回原值（沒有連線時只是存起來）
      if (c.stop.load()) {
        return;
      }
      const auto c1 = bridge_view::counters();
      const uint64_t d_bye = delta(bridge_view::byes(c1, VRIPC_BYE_RECONFIG), bridge_view::byes(c0, VRIPC_BYE_RECONFIG));
      const auto chk = p->last("ipcpeer"sv, "close-after-welcome-check"sv);
      const int64_t occupants = chk ? chk->get_int("occupants").value_or(-1) : -1;
      const int64_t intact = chk ? chk->get_int("intact").value_or(-1) : -1;
      const auto closed = p->last("ipcpeer"sv, "close-after-welcome"sv);
      const int64_t same_value = closed ? closed->get_int("occupiedSameValue").value_or(-1) : -1;
      const bool pass = w == probe_proc_t::wait_e::exited && reconfigured && cfg2_ok && g2 > g1 && d_bye >= 1 && occupants > 0 && intact == occupants;
      c.r.check(id, pass, std::format("gen={}->{} configAccepted={} byeReconfig=+{} occupants={} intact={} occupiedSameValue={} {}", g1, g2, cfg2_ok ? 1 : 0, d_bye, occupants, intact, same_value, peer_diag(*p)));
      if (pass && same_value == 0) {
        c.r.info("T2.ownership-strength"sv, "occupiedSameValue=0: the placeholder objects did not reuse the delivered handle values, so this run could not have caught a remote close (weaker evidence; rerun T2)"sv);
      }
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.never-read（sec-M8、§B.4）：peer 讀完 WELCOME 之後不再讀 pipe。先觀察 600 ms 的 bridge heartbeat，
     *        再一則接一則送 STATE（bridge_view::send_state_filler）直到 pipe 的輸出緩衝滿、寫入卡住：bridge 等 100 ms 就
     *        CancelIoEx → TEARDOWN（不送 BYE、不等對方關閉，永遠不 FlushFileBuffers）。
     *        判定：拆除原因是 write-failed、最後一次送出嘗試（卡住的那一則）到拆除完成 ≤ 300 ms、最後一次 heartbeat 到拆除
     *        完成 ≤ 300 ms、連線期間 heartbeat（每 100 ms）相鄰兩次更新的間隔 ≤ 250 ms（kHeartbeatMs×2＋排程餘裕）。
     * vr_probe：`--mode ipcpeer --seconds 12 --peer-never-read`
     */
    void t2_never_read(t2_ctx_t &c) {
      constexpr auto id = "T2.never-read"sv;
      constexpr int max_states = 4000;  ///< 每則 32 B：遠超過 4 KiB 的 pipe 緩衝；送完仍沒卡住就是 FAIL
      if (!wait_idle(c, id)) {
        return;
      }
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 12 --peer-never-read", true);
      if (!p) {
        return;
      }
      if (wait_until(8000ms, c.stop, []() {
            return bridge_view::status().connected;
          }) < 0) {
        bridge_view::revoke_peer();
        if (!c.stop.load()) {
          c.r.check(id, false, "peer never connected " + peer_diag(*p));
          relay(c.r, id, *p);
        }
        return;
      }
      const uint64_t gen = bridge_view::status().generation;
      const uint64_t teardowns0 = bridge_view::counters().teardowns;

      // 1 ms 輪詢（高解析度 waitable timer）：時間點夠準，又不會像 yield 迴圈那樣一直搶 bridge 的 mtx_
      // （status()／counters() 都要鎖；bridge 執行緒每 2 ms 在 process_commands 也要鎖）
      hr_tick_t tick;
      int64_t hb_value = bridge_view::status().server_hb;
      auto hb_at = steady::now();
      int64_t hb_max_gap_ms = 0;
      int hb_updates = 0;
      uint64_t states_seen = bridge_view::counters().states_sent;
      const uint64_t states0 = states_seen;
      std::optional<steady::time_point> last_attempt;
      std::optional<steady::time_point> torn_at;
      bool pending = false;
      int sent = 0;
      const auto t0 = steady::now();
      while (!c.stop.load() && steady::now() - t0 < 4s) {
        const auto now = steady::now();
        const auto s = bridge_view::status();
        const auto k = bridge_view::counters();
        if (s.server_hb != hb_value) {
          // 只量「兩次都親眼看到的更新」之間的間隔：第一次更新之前的那一段從握手的初始 heartbeat 算起，
          // 會含第一個 generation 在 bridge 執行緒上建 D3D device 與 ring 的時間（本機實測約 260 ms），不是穩態間隔
          if (hb_updates > 0) {
            hb_max_gap_ms = std::max<int64_t>(hb_max_gap_ms, std::chrono::duration_cast<std::chrono::milliseconds>(now - hb_at).count());
          }
          hb_value = s.server_hb;
          hb_at = now;
          ++hb_updates;
        }
        if (k.states_sent != states_seen) {
          states_seen = k.states_sent;
          last_attempt = now;
          pending = false;
        }
        if (k.teardowns != teardowns0) {
          torn_at = now;
          break;
        }
        // 先看 600 ms 的正常 heartbeat，之後才開始塞：上一則的送出嘗試被計數了才送下一則
        if (now - t0 >= 600ms && !pending && sent < max_states) {
          bridge_view::send_state_filler();
          pending = true;
          ++sent;
        }
        tick.sleep_1ms();
      }
      const auto st = bridge_view::status();
      p->terminate();
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      const auto ms_between = [](steady::time_point a, steady::time_point b) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
      };
      const bool write_failed = std::string_view {st.last_teardown_reason}.starts_with("write-failed"sv);
      const int64_t stall_ms = torn_at && last_attempt ? ms_between(*last_attempt, *torn_at) : -1;
      const int64_t hb_to_teardown_ms = torn_at ? ms_between(hb_at, *torn_at) : -1;
      const bool pass = torn_at && write_failed && stall_ms >= 0 && stall_ms <= 300 && hb_to_teardown_ms >= 0 && hb_to_teardown_ms <= 300 && hb_updates >= 3 && hb_max_gap_ms <= 250;
      c.r.check(id, pass, std::format("gen={} tornDown={} reason=\"{}\" statesSent={} (last one blocked) blockedToTeardownMs={} (limit 300) hbUpdates={} hbMaxGapMs={} (limit 250) hbLastToTeardownMs={} (limit 300) {}", gen, torn_at ? 1 : 0, st.last_teardown_reason, states_seen - states0, stall_ms, hb_updates, hb_max_gap_ms, hb_to_teardown_ms, peer_diag(*p)));
      relay(c.r, id, *p);
    }

    // ── 合成 tracking（T2.dual-writer；§F.5「合成 tracking」的最小版）──────────────
    // flags：VipleVr.h 的 VIPLE_VR_TRK_HMD|LEFT|RIGHT|PRESENCE（0x01|0x02|0x04|0x10）
    constexpr uint8_t k_trk_flags_synth = 0x01 | 0x02 | 0x04 | 0x10;
    /// 撕裂檢查圖樣的標記：R 控制器角速度 z = 12345 rad/s（真實資料不會出現）。vr_probe ipcpeer 的 tracking 讀取端
    /// 看到這個標記就逐欄核對 payload 與 sampleId（tools/vr_probe/ipcpeer.cpp 的 k_trk_pattern_marker，兩邊必須相同）。
    constexpr float k_trk_pattern_marker = 12345.0f;

    /// 一筆合成樣本：時基 QPC、target = now + predict（不經時鐘對映）；payload 各欄都由 sampleId 推得（撕裂檢查圖樣）
    vr::bridge::tracked_sample_t synth_sample(uint32_t id, uint16_t space_epoch) {
      vr::bridge::tracked_sample_t s;
      const int64_t now = platf::qpc_counter();
      const int64_t f = platf::qpc_frequency();
      const int64_t predict = f > 0 ? f / 90 : 0;
      s.sample_id = id;
      s.space_epoch = space_epoch;
      s.flags = k_trk_flags_synth;
      s.sample_time_ns = static_cast<uint64_t>(platf::qpc_ticks_to_ns(now));
      s.arrival_qpc = now;
      s.target_server_qpc = now + predict;
      s.predict_ns = static_cast<uint32_t>(platf::qpc_ticks_to_ns(predict));
      for (auto &pose : s.pose) {
        pose.pos[1] = 1.6f;
        pose.rot[3] = 1.0f;
      }
      s.pose[1].pos[0] = -0.2f;
      s.pose[2].pos[0] = 0.2f;
      s.pose[2].ang_vel[0] = static_cast<float>(id & 0xFFFFu);
      s.pose[2].ang_vel[1] = static_cast<float>(id >> 16);
      s.pose[2].ang_vel[2] = k_trk_pattern_marker;
      s.input[0].buttons = id;
      s.input[1].buttons = ~id;
      s.gaze_yaw_f16 = static_cast<uint16_t>(id ^ 0xA5A5u);
      s.gaze_pitch_f16 = static_cast<uint16_t>(id >> 16);
      return s;
    }

    /// 以 500 Hz（高解析度 waitable timer）寫到 until；sampleId 取自共用計數器。@return 呼叫 publish 的次數
    uint64_t synth_writer(std::atomic<uint32_t> &next_id, uint16_t space_epoch, steady::time_point until, const std::atomic<bool> &stop) {
      handle_t timer {CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
      if (!timer) {
        timer = handle_t {CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS)};
      }
      uint64_t calls = 0;
      while (!stop.load() && steady::now() < until) {
        bridge_view::publish(synth_sample(next_id.fetch_add(1), space_epoch));
        ++calls;
        LARGE_INTEGER due;
        due.QuadPart = -20000;  // 2 ms（100 ns 單位，相對）
        if (timer && SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) {
          WaitForSingleObject(timer.h, 100);
        } else {
          std::this_thread::sleep_for(2ms);
        }
      }
      return calls;
    }

    /**
     * @brief T2.dual-writer（K26、sec-M6、ops-M5）＋ T2.echo（§B.8 第 4 條）：READY 後 server 端兩條執行緒各以 500 Hz 呼叫
     *        vr::bridge::publish_tracking 10 s。sampleId 取自同一個計數器（模擬同一串樣本同時經 ENet 與 picoquic 兩條
     *        執行緒到達：進 writer mutex 的順序可能與 sampleId 相反，bridge 在臨界區內丟掉「不比上一筆新」的樣本）。
     *        同時跑 selftest 消費者：peer 把讀到的最新 sampleId 放進 descriptor 的 echo，消費端以 server 最近 2 s 發布過的
     *        集合驗證（sample_recent）。
     *        判定（dual-writer）：peer 的 tracking 讀取 reads>0、torn=0、backwards=0、撕裂圖樣 patternChecked>0 且
     *        patternBad=0（每一筆 payload 的各欄都與 sampleId 一致＝pose 沒有不連續）；bridge 有發布。
     *        判定（echo）：消費者有幀、0 不合格、echo 對上的幀 > 0。
     * vr_probe：`--mode ipcpeer --seconds 14 --peer-writers 2`
     */
    void t2_dual_writer(t2_ctx_t &c) {
      constexpr auto id = "T2.dual-writer"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 14 --peer-writers 2", true);
      if (!p) {
        return;
      }
      std::atomic<bool> run_stop {false};
      std::atomic<uint32_t> next_id {1};  // 0 在 echo 裡表示「沒有」
      uint64_t calls[2] = {};
      std::vector<std::thread> threads;
      std::optional<vr::bridge::consume_report_t> rep;
      bool started = false;
      bool wanted = false;
      bridge_view::status_t s0;
      const auto w = p->wait(22s, c.stop, [&]() {
        if (!started) {
          const auto s = bridge_view::status();
          if (s.connected && s.ready) {
            started = true;
            s0 = s;
            wanted = bridge_view::tracking_wanted();
            const auto until = steady::now() + 10s;
            threads.emplace_back([&]() {
              rep = bridge_view::consume(10500ms, run_stop);
            });
            for (int i = 0; i < 2; ++i) {
              threads.emplace_back([&, i, until]() {
                calls[i] = synth_writer(next_id, c.cfg.space_epoch, until, run_stop);
              });
            }
          }
        }
        return true;  // peer 結束前才印 tracking 統計，等它自己結束
      });
      run_stop.store(true);
      for (auto &t : threads) {
        t.join();
      }
      const auto s1 = bridge_view::status();
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      const auto trk = p->last("ipcpeer"sv, "tracking"sv);
      const int64_t reads = trk ? trk->get_int("reads").value_or(-1) : -1;
      const int64_t torn = trk ? trk->get_int("torn").value_or(-1) : -1;
      const int64_t backwards = trk ? trk->get_int("backwards").value_or(-1) : -1;
      const int64_t checked = trk ? trk->get_int("patternChecked").value_or(-1) : -1;
      const int64_t bad = trk ? trk->get_int("patternBad").value_or(-1) : -1;
      const uint64_t published = delta(s1.trk_published, s0.trk_published);
      const uint64_t dropped = delta(s1.trk_dropped, s0.trk_dropped);
      const bool pass = started && w == probe_proc_t::wait_e::exited && wanted && published > 0 && reads > 0 && torn == 0 && backwards == 0 && checked > 0 && bad == 0;
      c.r.check(id, pass, std::format("calls={}+{} published=+{} dropped=+{} (reordered between the two writers) peerReads={} torn={} backwards={} patternChecked={} patternBad={}{} {}", calls[0], calls[1], published, dropped, reads, torn, backwards, checked, bad, checked < 0 && trk ? " (vr_probe too old: rebuild and redeploy it)"sv : ""sv, peer_diag(*p)));

      const bool echo_ok = rep && rep->ok && rep->frames_copied > 0 && rep->frames_invalid == 0 && rep->frames_echo_matched > 0;
      c.r.check("T2.echo"sv, echo_ok, std::format("copied={} invalid={} echoMatched={} echoFallback={} fenceTimeout={} error=\"{}\"", rep ? rep->frames_copied : 0, rep ? rep->frames_invalid : 0, rep ? rep->frames_echo_matched : 0, rep ? rep->frames_echo_fallback : 0, rep ? rep->frames_fence_timeout : 0, rep ? rep->error : "no consumer report"s));
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.escalate（U30、sec-m9；只記錄）：peer 對 WELCOME 收到的三個 handle 嘗試 DuplicateHandle(self→self, *_ALL_ACCESS)。
     *        granted＝不具名物件沒有保存 server 傳入的 SD，vrserver 可以自己升權；最小權限複製只是衛生措施，對 SYSTEM
     *        沒有提權。三個結果都收到就記 INFO；收不到（情境沒跑起來）才 FAIL。
     * vr_probe：`--mode ipcpeer --seconds 3 --peer-no-render --peer-escalate`（結果在 ipc_client 的 log：`drv peer escalate obj= result= err=`）
     */
    void t2_escalate(t2_ctx_t &c) {
      constexpr auto id = "T2.escalate"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      const auto c0 = bridge_view::counters();
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 3 --peer-no-render --peer-escalate", true);
      if (!p) {
        return;
      }
      p->wait(10s, c.stop);
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      const auto c1 = bridge_view::counters();
      std::string results;
      int reported = 0;
      for (const auto &kv : p->all("drv"sv, "peer"sv)) {
        const auto obj = kv.get("obj");
        const auto res = kv.get("result");
        if (obj && res) {
          results += std::format(" {}={}(err={})", *obj, *res, kv.get("err").value_or("?"));
          ++reported;
        }
      }
      const uint64_t d_hs = delta(c1.handshakes_ok, c0.handshakes_ok);
      if (d_hs >= 1 && reported >= 3) {
        c.r.info(id, "DuplicateHandle(self->self, *_ALL_ACCESS) on the WELCOME handles:" + results + " (U30: granted = the unnamed object kept no SD; least-privilege duplication is hygiene only)");
      } else {
        c.r.check(id, false, std::format("peer did not report the escalation results (handshakes=+{} reported={}) {}", d_hs, reported, peer_diag(*p)));
      }
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.second-instance（E-1、K3）：以主控台使用者的 token 模擬後，對同名 VR pipe 呼叫 CreateNamedPipeW（加掛 instance）
     *        → 必須 ERROR_ACCESS_DENIED：使用者 ACE 0x12019b 不含 FILE_CREATE_PIPE_INSTANCE。ERROR_PIPE_BUSY 代表存取檢查
     *        已經通過、只是被 nMaxInstances=1 擋下（DACL 退化，scout E §2.4 的實測），算 FAIL。另以 SYSTEM 帶
     *        FILE_FLAG_FIRST_PIPE_INSTANCE 呼叫一次（名稱已存在）→ 也必須失敗（ACCESS_DENIED）。
     *        萬一建立成功，立刻關掉（不留 rogue instance）並判 FAIL。
     */
    void t2_second_instance(t2_ctx_t &c) {
      constexpr auto id = "T2.second-instance"sv;
      DWORD self = 0;
      ProcessIdToSessionId(GetCurrentProcessId(), &self);
      const std::wstring name = std::wstring {VRIPC_PIPE_NAME_PREFIX} + std::to_wstring(self);
      const DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
      const DWORD pipe_mode = PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;

      handle_t token {platf::retrieve_users_token(false)};
      if (!token) {
        c.r.check(id, false, "no console user token"sv);
        return;
      }
      DWORD user_err = 0;
      bool user_created = false;
      DWORD imp_err = 0;
      {
        platf::impersonation_guard guard {token.h};
        if (!guard.valid()) {
          imp_err = guard.error();
        } else {
          HANDLE h = CreateNamedPipeW(name.c_str(), open_mode, pipe_mode, 1, 4096, 4096, 0, nullptr);
          if (h == INVALID_HANDLE_VALUE) {
            user_err = GetLastError();
          } else {
            user_created = true;
            CloseHandle(h);
          }
        }
      }  // guard 在這裡 RevertToSelf（失敗就 terminate）
      if (imp_err) {
        c.r.check(id, false, std::format("impersonating the console user failed err={}", imp_err));
        return;
      }
      DWORD sys_err = 0;
      bool sys_created = false;
      {
        HANDLE h = CreateNamedPipeW(name.c_str(), open_mode | FILE_FLAG_FIRST_PIPE_INSTANCE, pipe_mode, 1, 4096, 4096, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
          sys_err = GetLastError();
        } else {
          sys_created = true;
          CloseHandle(h);
        }
      }
      // SYSTEM 有完整權限，存取檢查必過；nMaxInstances=1 已滿時 Windows 先回 ERROR_PIPE_BUSY（host 實測 231），
      // 不一定走到 FIRST_PIPE_INSTANCE 的 ERROR_ACCESS_DENIED。兩者都代表「建不出第二個實例」，都算通過。
      // 使用者那一次只接受 5：231 代表 ACE 給了 FILE_CREATE_PIPE_INSTANCE、只是被實例數擋下（DACL 退化）。
      const bool sys_blocked = !sys_created && (sys_err == ERROR_ACCESS_DENIED || sys_err == ERROR_PIPE_BUSY);
      const bool pass = !user_created && user_err == ERROR_ACCESS_DENIED && sys_blocked;
      c.r.check(id, pass, std::format("user={} (expect err-5; err-231 = the ACE grants FILE_CREATE_PIPE_INSTANCE) systemFirstInstance={} (expect err-5 or err-231)", user_created ? "CREATED"s : std::format("err-{}", user_err), sys_created ? "CREATED"s : std::format("err-{}", sys_err)));
    }

    /**
     * @brief 一次「應該被 REJECT」的連線：看到對應的 reject 計數增加就結束 peer（避免它重試讓退避 streak 越疊越高）。
     */
    void t2_expect_reject(t2_ctx_t &c, std::string_view id, const std::string &args, bool allow, unsigned reason, std::string_view reason_name) {
      if (!wait_idle(c, id)) {
        return;
      }
      const auto c0 = bridge_view::counters();
      auto p = launch_peer(c, id, args, allow);
      if (!p) {
        return;
      }
      const auto t0 = steady::now();
      int64_t reject_ms = -1;
      p->wait(15s, c.stop, [&]() {
        if (delta(bridge_view::rejects(bridge_view::counters(), reason), bridge_view::rejects(c0, reason)) >= 1) {
          reject_ms = ms_since(t0);
          return false;
        }
        return true;
      });
      if (allow) {
        bridge_view::revoke_peer();
      }
      if (c.stop.load()) {
        return;
      }
      const auto c1 = bridge_view::counters();
      const uint64_t d_rej = delta(bridge_view::rejects(c1, reason), bridge_view::rejects(c0, reason));
      const uint64_t d_hs = delta(c1.handshakes_ok, c0.handshakes_ok);
      // peer 端若印出 reject 行，一併核對 reason（ABI_MISMATCH 的 detail 必須是 server 的 abi）
      std::string peer_seen = "n/a";
      bool peer_ok = true;
      for (const auto &line : p->lines()) {
        const auto kv = parse_probe_kv(line);
        if (kv.tag == "reject" || kv.sub == "reject") {
          const auto pr = kv.get_int("reason").value_or(-1);
          const auto pd = kv.get_int("detail").value_or(-1);
          peer_seen = std::format("reason={} detail={}", pr, pd);
          peer_ok = pr == static_cast<int64_t>(reason) && (reason != VRIPC_REJ_ABI_MISMATCH || pd == static_cast<int64_t>(VRIPC_ABI_VERSION));
        }
      }
      const bool pass = d_rej >= 1 && d_hs == 0 && peer_ok;
      c.r.check(id, pass, std::format("reject={} +{} afterMs={} handshakes=+{} peerSaw=[{}] streak={} {}", reason_name, d_rej, reject_ms, d_hs, peer_seen, c1.reject_streak, peer_diag(*p)));
      relay(c.r, id, *p);
    }

    /**
     * @brief T2.impostor：allow_selftest_peer 登記的是另一個（誘餌）行程，實際連線的是沒登記的 vr_probe
     *        → 映像不是 vrserver.exe、PID 也不是登記的那個 → REJECT(IDENTITY)。
     * 誘餌：`--mode ipcpeer --seconds 30 --peer-no-render --pipe \\.\pipe\VipleStreamVR-selftest-decoy`
     *       （連一個不存在的 pipe，永遠不會碰到 bridge，只是活著佔住放行名額）；
     * 冒名者：`--mode ipcpeer --seconds 15`。
     */
    void t2_impostor(t2_ctx_t &c) {
      constexpr auto id = "T2.impostor"sv;
      auto decoy = launch_peer(c, id, "--mode ipcpeer --seconds 30 --peer-no-render --pipe \\\\.\\pipe\\VipleStreamVR-selftest-decoy", true);
      if (!decoy) {
        return;
      }
      t2_expect_reject(c, id, "--mode ipcpeer --seconds 15", false, VRIPC_REJ_IDENTITY, "IDENTITY"sv);
      bridge_view::revoke_peer();
      decoy->terminate();
    }

    /**
     * @brief T2.rate-limit（sec-m18、§B.1 第 7 步）：連續 10 次錯誤的 HELLO（大小 100 B → REJECT(BAD_MESSAGE)），每次一個新的
     *        vr_probe（新行程沒有 driver 端的退避，server 一回到 ConnectNamedPipe 就連進來），看到 reject 計數增加就結束它。
     *        判定：10 次都被拒、streak 每次 +1、相鄰兩次 reject 的間隔 ≥ 前一次 streak 的退避（1、2、4、8、10、10… s），
     *        容許 −50 ms（以 20 ms 輪詢量）。全程約 65–75 s。之後由 T2.recover-after-rate-limit 驗 streak 歸零。
     * vr_probe：每次 `--mode ipcpeer --seconds 30 --hello-size 100`
     */
    void t2_rate_limit(t2_ctx_t &c) {
      constexpr auto id = "T2.rate-limit"sv;
      constexpr int attempts = 10;
      if (!wait_idle(c, id)) {
        return;
      }
      struct seen_t {
        steady::time_point at;
        uint32_t streak;
      };

      std::vector<seen_t> seen;
      std::string failure;
      for (int i = 0; i < attempts && !c.stop.load(); ++i) {
        const auto c0 = bridge_view::counters();
        const int64_t backoff_s = seen.empty() ? vr::bridge::reject_backoff_sec(c0.reject_streak) : vr::bridge::reject_backoff_sec(seen.back().streak);
        auto p = launch_peer(c, id, "--mode ipcpeer --seconds 30 --hello-size 100", true);
        if (!p) {
          return;  // launch_peer 已回報 FAIL
        }
        std::optional<seen_t> got;
        p->wait(std::chrono::seconds(backoff_s + 10), c.stop, [&]() {
          const auto k = bridge_view::counters();
          if (delta(bridge_view::rejects(k, VRIPC_REJ_BAD_MESSAGE), bridge_view::rejects(c0, VRIPC_REJ_BAD_MESSAGE)) >= 1) {
            got = seen_t {steady::now(), k.reject_streak};
            return false;
          }
          return true;
        });
        bridge_view::revoke_peer();
        if (c.stop.load()) {
          return;
        }
        if (!got) {
          failure = std::format(" attempt={} noRejectWithin={}s {}", i + 1, backoff_s + 10, peer_diag(*p));
          relay(c.r, id, *p);
          break;
        }
        seen.push_back(*got);
        if (i + 1 == attempts) {
          relay(c.r, id, *p);
        }
      }
      if (c.stop.load()) {
        return;
      }
      bool ok = failure.empty() && seen.size() == static_cast<std::size_t>(attempts);
      std::string streaks;
      std::string gaps;
      for (std::size_t i = 0; i < seen.size(); ++i) {
        streaks += std::format("{}{}", i ? "," : "", seen[i].streak);
        if (i == 0) {
          continue;
        }
        const int64_t gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(seen[i].at - seen[i - 1].at).count();
        const int64_t need_ms = vr::bridge::reject_backoff_sec(seen[i - 1].streak) * 1000;
        const bool gap_ok = gap_ms >= need_ms - 50;
        const bool streak_ok = seen[i].streak == seen[i - 1].streak + 1 || (seen[i - 1].streak >= 1000 && seen[i].streak == 1000);
        ok = ok && gap_ok && streak_ok;
        gaps += std::format("{}{}ms/{}{}", i > 1 ? "," : "", gap_ms, need_ms, gap_ok ? "" : "!");
      }
      c.r.check(id, ok, std::format("rejects={}/{} streaks=[{}] gapsMs/needMs=[{}]{}", seen.size(), attempts, streaks, gaps, failure));
    }

    /**
     * @brief T2.recover：連續 REJECT 之後（server 退避 1→2→4→8→10 s，sec-m18），正常 peer 仍能在退避結束後握手成功，streak 歸零。
     *        跑兩次：REJECT 類情境之後（T2.recover）與限速之後（T2.recover-after-rate-limit，最多等 10 s 的退避）。
     * vr_probe：`--mode ipcpeer --seconds 15`
     */
    void t2_recover(t2_ctx_t &c, std::string_view id) {
      if (!wait_idle(c, id)) {
        return;
      }
      const auto c0 = bridge_view::counters();
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 15", true);
      if (!p) {
        return;
      }
      const auto t0 = steady::now();
      int64_t ready_ms = -1;
      p->wait(18s, c.stop, [&]() {
        const auto s = bridge_view::status();
        if (s.connected && s.ready) {
          ready_ms = ms_since(t0);
          return false;
        }
        return true;
      });
      bridge_view::revoke_peer();
      if (c.stop.load()) {
        return;
      }
      wait_until(1000ms, c.stop, []() {
        return !bridge_view::status().connected;
      });
      const auto c1 = bridge_view::counters();
      const bool pass = ready_ms >= 0 && delta(c1.handshakes_ok, c0.handshakes_ok) >= 1 && c1.reject_streak == 0;
      c.r.check(id, pass, std::format("streakBefore={} readyMs={} (includes backoff) streakAfter={} {}", c0.reject_streak, ready_ms, c1.reject_streak, peer_diag(*p)));
      relay(c.r, id, *p);
    }

    // ── T6 ──────────────────────────────────────────────────────────────

    void t6_qpc(reporter_t &r) {
      constexpr int64_t max = std::numeric_limits<int64_t>::max();
      constexpr int64_t min = std::numeric_limits<int64_t>::min();
      const int64_t f = platf::qpc_frequency();
      r.check("T6.qpc-freq"sv, f > 0 && f == platf::vr_clock_frequency(), std::format("qpf={} vr_clock_freq={}", f, platf::vr_clock_frequency()));
      if (f <= 0) {
        return;
      }

      // S1-01：qpc_time_difference(now, now − QPF) ∈ [0.999, 1.001] s（整數公式下恰好 1e9 ns）
      const int64_t now = platf::qpc_counter();
      const int64_t one_s = platf::qpc_time_difference(now, now - f).count();
      r.check("T6.qpc-1s"sv, one_s >= 999'000'000 && one_s <= 1'001'000'000, std::format("ns={}", one_s));
      r.check("T6.qpc-1tick"sv, platf::qpc_ticks_to_ns(1) == 1'000'000'000 / f, std::format("ns={} expect={}", platf::qpc_ticks_to_ns(1), 1'000'000'000 / f));

      // 負值對稱（qpc_ticks_to_ns(-t) == -qpc_ticks_to_ns(t)）
      const int64_t sym_inputs[] = {1, 7, f - 1, f + 1, f * 3600 + 12345, int64_t {1} << 40, max, min + 1};
      int sym_bad = 0;
      for (const auto t : sym_inputs) {
        if (platf::qpc_ticks_to_ns(-t) != -platf::qpc_ticks_to_ns(t)) {
          ++sym_bad;
        }
      }
      r.check("T6.qpc-symmetric"sv, sym_bad == 0, std::format("inputs={} bad={}", std::size(sym_inputs), sym_bad));

      // 飽和：極端輸入不溢位、對稱飽和到 ±INT64_MAX（V1 的 qpc_test 同一組）
      struct sat_t {
        const char *what;
        int64_t got;
        int64_t expect;
      };

      const sat_t sats[] = {
        {"diff(INT64_MAX,0)", platf::qpc_time_difference(max, 0).count(), max},
        {"diff(0,INT64_MAX)", platf::qpc_time_difference(0, max).count(), -max},
        {"ticks(INT64_MAX)", platf::qpc_ticks_to_ns(max), max},
        {"ticks(INT64_MIN)", platf::qpc_ticks_to_ns(min), -max},
        {"from100ns(INT64_MAX)", platf::qpc_from_100ns(max), max},
        {"from100ns(INT64_MIN)", platf::qpc_from_100ns(min), -max},
      };
      std::string sat_detail;
      bool sat_ok = true;
      for (const auto &s : sats) {
        const bool ok = s.got == s.expect;
        sat_ok = sat_ok && ok;
        if (!ok) {
          sat_detail += std::format(" {}={}(expect {})", s.what, s.got, s.expect);
        }
      }
      r.check("T6.qpc-saturation"sv, sat_ok, sat_ok ? std::format("cases={}", std::size(sats)) : "bad:" + sat_detail);

      // 一年份的 tick 不溢位、數值精確
      const int64_t year_s = 31'536'000;
      r.check("T6.qpc-year"sv, platf::qpc_ticks_to_ns(f * year_s) == year_s * 1'000'000'000, std::format("ns={}", platf::qpc_ticks_to_ns(f * year_s)));

      // 100 ns → QPC tick（WGC SystemRelativeTime，ops-m3）
      const bool h_ok = platf::qpc_from_100ns(10'000'000) == f && platf::qpc_from_100ns(-10'000'000) == -f;
      int rt_bad = 0;
      for (const int64_t t100 : {int64_t {1}, int64_t {12345}, int64_t {10'000'001}, int64_t {864'000'000'000}}) {
        const int64_t ns = platf::qpc_ticks_to_ns(platf::qpc_from_100ns(t100));
        const int64_t err = ns - t100 * 100;
        if (err < -(1'000'000'000 / f + 100) || err > 1'000'000'000 / f + 100) {
          ++rt_bad;
        }
      }
      r.check("T6.qpc-100ns"sv, h_ok && rt_bad == 0, std::format("oneSecond={} roundTripBad={}", platf::qpc_from_100ns(10'000'000), rt_bad));
    }

    /**
     * @brief T6.frame-desc：§B.8 第 2–6 條的純函式（vr::bridge::check_frame_desc）逐條餵不合格的 descriptor，
     *        每一條都要得到對應的原因；邊界值（跳幅剛好 1024、剛好 ±1 s）要被接受。
     */
    void t6_frame_desc(reporter_t &r) {
      using vr::bridge::frame_invalid_e;
      vr::bridge::frame_check_ctx_t ctx;
      ctx.generation = 5;
      ctx.have_last_frame = true;
      ctx.last_frame_id = 100;
      ctx.last_fence = 200;
      ctx.now_qpc = 1'000'000'000;
      ctx.qpc_frequency = 10'000'000;
      ctx.layout_epoch = 2;
      ctx.space_epoch = 3;

      vripc_frame_desc_t good {};
      good.generation = 5;
      good.frame_id = 101;
      good.fence_value = 201;
      good.tex_idx = 2;
      good.flags = VRIPC_FRM_POSE_VALID | VRIPC_FRM_ECHO_MATCHED | VRIPC_FRM_SPACE_DELTA;
      good.render_pos[1] = 1.6f;
      good.render_rot[3] = 1.0f;
      good.layer_count = 2;
      good.t_target_qpc = ctx.now_qpc + 110'000;
      good.present_qpc = ctx.now_qpc;
      good.submit_qpc = ctx.now_qpc;
      good.layout_epoch = 2;
      good.space_epoch = 3;

      struct case_t {
        const char *name;
        std::function<void(vripc_frame_desc_t &, vr::bridge::frame_check_ctx_t &)> mutate;
        frame_invalid_e expect;
      };

      const float nan = std::numeric_limits<float>::quiet_NaN();
      const case_t cases[] = {
        {"good", [](auto &, auto &) {}, frame_invalid_e::none},
        {"jump-1024", [](auto &d, auto &) { d.frame_id = 100 + 1024; d.fence_value = 200 + 1024; }, frame_invalid_e::none},
        {"time-edge", [&](auto &d, auto &c) { d.present_qpc = c.now_qpc - c.qpc_frequency; d.submit_qpc = c.now_qpc + c.qpc_frequency; }, frame_invalid_e::none},
        {"generation", [](auto &d, auto &) { d.generation = 6; }, frame_invalid_e::generation},
        {"frame-repeat", [](auto &d, auto &) { d.frame_id = 100; }, frame_invalid_e::frame_id},
        {"frame-jump", [](auto &d, auto &) { d.frame_id = 100 + 1025; }, frame_invalid_e::frame_id},
        {"frame-first-zero", [](auto &d, auto &c) { c.have_last_frame = false; d.frame_id = 0; }, frame_invalid_e::frame_id},
        {"fence-repeat", [](auto &d, auto &) { d.fence_value = 200; }, frame_invalid_e::fence},
        {"fence-jump", [](auto &d, auto &) { d.fence_value = 200 + 1025; }, frame_invalid_e::fence},
        {"fence-max", [](auto &d, auto &) { d.fence_value = UINT64_MAX; }, frame_invalid_e::fence},
        {"tex-idx", [](auto &d, auto &) { d.tex_idx = VRIPC_TEX_COUNT; }, frame_invalid_e::tex_idx},
        {"flags", [](auto &d, auto &) { d.flags |= 0x40u; }, frame_invalid_e::flags},
        {"layers", [](auto &d, auto &) { d.layer_count = 17; }, frame_invalid_e::layers},
        {"pos-nan", [&](auto &d, auto &) { d.render_pos[0] = nan; }, frame_invalid_e::pose},
        {"rot-nan", [&](auto &d, auto &) { d.render_rot[2] = nan; }, frame_invalid_e::pose},
        {"rot-norm", [](auto &d, auto &) { d.render_rot[3] = 0.5f; }, frame_invalid_e::pose},
        {"pos-far", [](auto &d, auto &) { d.render_pos[0] = 200.0f; }, frame_invalid_e::pose},
        {"time-future", [](auto &d, auto &c) { d.present_qpc = c.now_qpc + 2 * c.qpc_frequency; }, frame_invalid_e::time},
        {"time-past", [](auto &d, auto &c) { d.t_target_qpc = c.now_qpc - 2 * c.qpc_frequency; }, frame_invalid_e::time},
        {"layout-epoch", [](auto &d, auto &) { d.layout_epoch = 1; }, frame_invalid_e::epoch},
        {"space-epoch", [](auto &d, auto &) { d.space_epoch = 4; }, frame_invalid_e::epoch},
      };

      std::string bad;
      for (const auto &tc : cases) {
        auto d = good;
        auto c2 = ctx;
        tc.mutate(d, c2);
        const auto got = vr::bridge::check_frame_desc(d, c2);
        if (got != tc.expect) {
          bad += std::format(" {}:{}!={}", tc.name, vr::bridge::frame_invalid_name(got), vr::bridge::frame_invalid_name(tc.expect));
        }
      }
      r.check("T6.frame-desc"sv, bad.empty(), bad.empty() ? std::format("cases={}", std::size(cases)) : "bad:" + bad);
    }

    std::vector<std::string> abi_rows_of(const probe_proc_t &p) {
      std::vector<std::string> rows;
      constexpr std::string_view row_tag = "abi-row "sv;
      for (const auto &l : p.lines()) {
        if (std::string_view {l}.starts_with(row_tag)) {
          rows.emplace_back(std::string_view {l}.substr(row_tag.size()));
        }
      }
      return rows;
    }

    /**
     * @brief T6.probe-unit（K16）與 T6.abi-table。
     * - unit：`vr_probe --mode unit`，期望 `unit pass=<n> fail=<n>`、exit 0（driver 端純模組：fov_to_rect、virtual_vsync、
     *   pose_history 與 ipc_client 的迴路測試）。
     * - ABI 表：server（GCC）與 vr_probe（MSVC）各自印 vr_ipc_abi_table.h 的 305 列，逐字比對。vr_probe 在 ipcpeer 與 unit
     *   開頭印 `abi-row <key>=<value>` 各一行、最後 `abi-rows count=<n> digest=<16 hex>`；unit 的輸出沒有表時，另跑一次
     *   連不存在的 pipe 的 ipcpeer（`--peer-no-render --pipe \\.\pipe\VipleStreamVR-selftest-abi`，不碰 bridge）取表。
     */
    void t6_probe(reporter_t &r, const std::filesystem::path &probe, const std::atomic<bool> &stop) {
      probe_proc_t u {probe, "--mode unit"};
      if (!u.ok()) {
        r.check("T6.probe-unit"sv, false, "launch-failed " + u.error());
        r.check("T6.abi-table"sv, false, "launch-failed " + u.error());
        return;
      }
      const auto w = u.wait(120s, stop);
      if (stop.load()) {
        return;
      }
      const auto kv = u.last("unit"sv);
      const int64_t pass_n = kv ? kv->get_int("pass").value_or(-1) : -1;
      const int64_t fail_n = kv ? kv->get_int("fail").value_or(-1) : -1;
      const auto code = u.exit_code();
      r.check("T6.probe-unit"sv, w == probe_proc_t::wait_e::exited && kv && pass_n > 0 && fail_n == 0 && code && *code == 0, std::format("pass={} fail={} {}", pass_n, fail_n, peer_diag(u)));
      relay(r, "T6.probe-unit"sv, u);

      std::vector<std::string> server_rows;
      server_rows.reserve(vripc_abi_table::k_row_count);
      for (const auto &row : vripc_abi_table::k_rows) {
        server_rows.push_back(vripc_abi_table::format_row(row));
      }
      const auto server_digest = std::format("{:016x}", vripc_abi_table::table_digest());
      for (const auto &row : server_rows) {
        r.verbose("T6.abi-row"sv, row);
      }
      r.info("T6.abi-server"sv, std::format("rows={} digest={}", server_rows.size(), server_digest));

      auto probe_rows = abi_rows_of(u);
      auto tail = u.last("abi-rows"sv);
      std::string source = "unit";
      std::unique_ptr<probe_proc_t> q;
      if (probe_rows.empty()) {
        q = std::make_unique<probe_proc_t>(probe, "--mode ipcpeer --seconds 2 --peer-no-render --pipe \\\\.\\pipe\\VipleStreamVR-selftest-abi");
        if (q->ok()) {
          q->wait(15s, stop);
          if (stop.load()) {
            return;
          }
          probe_rows = abi_rows_of(*q);
          tail = q->last("abi-rows"sv);
        }
        source = "ipcpeer";
      }
      const std::string probe_digest = tail ? tail->get("digest").value_or("none") : "none"s;

      std::size_t mismatches = 0;
      const std::size_t n = std::max(server_rows.size(), probe_rows.size());
      for (std::size_t i = 0; i < n; ++i) {
        const std::string &a = i < server_rows.size() ? server_rows[i] : std::string {};
        const std::string &b = i < probe_rows.size() ? probe_rows[i] : std::string {};
        if (a != b) {
          if (++mismatches <= 20) {
            r.info("T6.abi-diff"sv, std::format("row={} server={} probe={}", i, a.empty() ? "<missing>"s : a, b.empty() ? "<missing>"s : b));
          }
        }
      }
      const bool pass = mismatches == 0 && probe_rows.size() == server_rows.size() && probe_digest == server_digest;
      r.check("T6.abi-table"sv, pass, std::format("source={} serverRows={} probeRows={} mismatches={} serverDigest={} probeDigest={}", source, server_rows.size(), probe_rows.size(), mismatches, server_digest, probe_digest));
    }

    // ── M1b V3：T1／T1b／T2.live ────────────────────────────────────────

    double pct(std::vector<double> v, double q) {
      if (v.empty()) {
        return 0;
      }
      std::sort(v.begin(), v.end());
      return v[std::min(v.size() - 1, (size_t) (q * (double) (v.size() - 1) + 0.5))];
    }

    /// T1／T1b 的 VR 探測形狀：預設每眼 1728²＠90 Hz、HEVC（設計 §C.6、一頁摘要第 5 點）
    ::video::vr_probe_shape_t t1_shape() {
      ::video::vr_probe_shape_t s;
      s.packed_w = 3456;
      s.packed_h = 1728;
      s.hz = 90;
      s.codec_mask = 0x2;
      s.ir_frames = config::vr.intra_refresh_frames;
      return s;
    }

    /// 探測後要被還原的桌面探測狀態（video.h 對外可見的部分）
    struct desktop_probe_view_t {
      int hevc_mode;
      int av1_mode;
      bool ref_frames_invalidation;
      std::array<bool, 3> yuv444;
      bool vr_intra_refresh;

      static desktop_probe_view_t now() {
        return {::video::active_hevc_mode, ::video::active_av1_mode, ::video::last_encoder_probe_supported_ref_frames_invalidation, ::video::last_encoder_probe_supported_yuv444_for_codec, ::video::last_encoder_probe_supported_vr_intra_refresh};
      }

      bool operator==(const desktop_probe_view_t &) const = default;

      std::string str() const {
        return std::format("hevcMode={} av1Mode={} rfi={} yuv444={}{}{} vrIntraRefresh={}", hevc_mode, av1_mode, ref_frames_invalidation ? 1 : 0, yuv444[0] ? 1 : 0, yuv444[1] ? 1 : 0, yuv444[2] ? 1 : 0, vr_intra_refresh ? 1 : 0);
      }
    };

    /// VR 形狀探測（vr_probe_scope 內；離開時還原桌面探測狀態）。@return probe_encoders 的回傳值與 VR intra refresh 結果
    std::pair<int, bool> vr_probe_once() {
      ::video::vr_probe_scope scope;
      const auto shape = t1_shape();
      const int rc = ::video::probe_encoders(1, &shape);
      return {rc, ::video::last_encoder_probe_supported_vr_intra_refresh};
    }

    /// XML 第一個差異點附近的元素名（不印值，避免把 uniqueid 之類的欄位寫進 log）
    std::string first_diff(const std::string &a, const std::string &b) {
      size_t i = 0;
      while (i < a.size() && i < b.size() && a[i] == b[i]) {
        ++i;
      }
      const auto lt = a.rfind('<', i);
      std::string elem;
      if (lt != std::string::npos) {
        auto end = a.find_first_of(" >/", lt + 1);
        elem = a.substr(lt + 1, (end == std::string::npos ? a.size() : end) - lt - 1);
      }
      return std::format("offset={} lenBefore={} lenAfter={} element=<{}>", i, a.size(), b.size(), elem);
    }

    /**
     * @brief T2.live（M1b V3、§F.5）：peer（ipcpeer）當 driver，selftest 自己建 display_vr_t（live）跑 capture() 60 s；
     *        影像池 3 張、push 回呼只做統計（設計 §F.5「selftest consumer」）。
     *        判定：消費率（copied ÷ 本 display 看到的 frame_id 範圍）≥ 99%、evtToPush p95 ≤ 1.5 ms、0 不合格、0 fence 遺失。
     * vr_probe：`--mode ipcpeer --seconds 80`
     */
    void t2_live(t2_ctx_t &c) {
      constexpr auto id = "T2.live"sv;
      if (!wait_idle(c, id)) {
        return;
      }
      auto p = launch_peer(c, id, "--mode ipcpeer --seconds 80", true);
      if (!p) {
        return;
      }
      auto cleanup = util::fail_guard([&]() {
        p->terminate();
        bridge_view::revoke_peer();
      });
      if (wait_until(15000ms, c.stop, []() {
            const auto s = bridge_view::status();
            return s.connected && s.ready;
          }) < 0) {
        if (!c.stop.load()) {
          c.r.check(id, false, "peer never became READY " + peer_diag(*p));
          relay(c.r, id, *p);
        }
        return;
      }

      ::video::config_t cfg {};
      cfg.width = (int) c.cfg.packed_width;
      cfg.height = (int) c.cfg.packed_height;
      cfg.framerate = (int) (c.cfg.refresh_mhz / 1000);
      cfg.captureSource = 1;
      auto disp = std::make_shared<platf::dxgi::display_vr_t>();
      if (disp->init(cfg)) {
        c.r.check(id, false, "display_vr_t init failed (see [VIPLE-VR-CAP] in sunshine.log)"sv);
        return;
      }

      std::vector<std::shared_ptr<platf::img_t>> pool(3);
      uint64_t pushed = 0;
      const auto t0 = std::chrono::steady_clock::now();
      auto pull = [&](std::shared_ptr<platf::img_t> &out) -> bool {
        out.reset();
        while (!c.stop.load()) {
          for (auto &slot : pool) {
            if (!slot) {
              slot = disp->alloc_img();
            }
            if (slot.use_count() == 1) {
              out = slot;
              out->frame_timestamp.reset();
              out->vr.reset();
              return true;
            }
          }
          std::this_thread::sleep_for(1ms);
        }
        return false;
      };
      auto push = [&](std::shared_ptr<platf::img_t> &&img, bool captured) -> bool {
        if (captured && img) {
          ++pushed;
        }
        img.reset();
        return !c.stop.load() && std::chrono::steady_clock::now() - t0 < 60s;
      };
      bool cursor = false;
      const auto status = disp->capture(push, pull, &cursor);
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
      if (c.stop.load()) {
        return;
      }
      const auto s = disp->stats();
      const uint64_t seen = s.last_frame_id >= s.first_frame_id && s.first_frame_id != 0 ? s.last_frame_id - s.first_frame_id + 1 : 0;
      const double consumption = seen ? (double) s.copied / (double) seen : 0.0;
      const double evt_p50 = pct(s.evt_to_push_ms, 0.5), evt_p95 = pct(s.evt_to_push_ms, 0.95), evt_max = pct(s.evt_to_push_ms, 1.0);
      const double pr_p50 = pct(s.present_to_push_ms, 0.5), pr_p95 = pct(s.present_to_push_ms, 0.95);
      const bool pass = status == platf::capture_e::ok && s.copied > 0 && consumption >= 0.99 && evt_p95 <= 1.5 && s.invalid == 0 && s.fence_lost == 0;
      c.r.check(id, pass, std::format("ms={} status={} gen={} copied={} seen={} consumption={:.4f} skipped={} stale={} fenceTimeout={} invalid={} torn={} fenceLost={} black={} pushed={} evtToPushMs p50={:.3f} p95={:.3f} max={:.3f} presentToPushMs p50={:.3f} p95={:.3f} {}", elapsed_ms, (int) status, s.generation, s.copied, seen, consumption, s.skipped, s.stale, s.fence_timeout, s.invalid, s.torn, s.fence_lost, s.black, pushed, evt_p50, evt_p95, evt_max, pr_p50, pr_p95, peer_diag(*p)));
      disp.reset();
      relay(c.r, id, *p);
    }

    // ── T4（M1b V4、§F.5）：--manual-steamvr ─────────────────────────────────────

    /// T4 的 session config：每眼 1728×1728@90（與 PoC-10 的證據行 3456x1728 90.0Hz 相同）、不對稱 FOV（驗 GetProjectionRaw 對映）
    vripc_session_config_t make_config_t4(const adapter_info_t &a) {
      vripc_session_config_t c = make_config(a, 1, 1);
      c.eye_width = 1728;
      c.eye_height = 1728;
      c.packed_width = 3456;
      c.packed_height = 1728;
      const float fl[4] = {-1.0f, 0.9f, 0.95f, -1.05f};  // left, right, up, down
      const float fr[4] = {-0.9f, 1.0f, 0.95f, -1.05f};
      std::copy(std::begin(fl), std::end(fl), std::begin(c.fov_tan[0]));
      std::copy(std::begin(fr), std::end(fr), std::begin(c.fov_tan[1]));
      return c;
    }

    /// 執行中的 vrserver.exe 映像（第一個找到的）；沒有回空字串
    std::wstring running_vrserver_image() {
      handle_t snap {CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
      if (!snap) {
        return {};
      }
      PROCESSENTRY32W pe {};
      pe.dwSize = sizeof(pe);
      for (BOOL ok = Process32FirstW(snap.h, &pe); ok; ok = Process32NextW(snap.h, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"vrserver.exe") != 0) {
          continue;
        }
        handle_t p {OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID)};
        if (!p) {
          continue;
        }
        std::wstring buf(1024, L'\0');
        DWORD n = static_cast<DWORD>(buf.size());
        if (QueryFullProcessImageNameW(p.h, 0, buf.data(), &n)) {
          buf.resize(n);
          return buf;
        }
      }
      return {};
    }

    /// 主控台使用者的 openvrpaths.vrpath → runtime[0]\bin\win64\vrserver.exe（只讀；以使用者 token 解析 LocalAppData）
    std::wstring vrserver_image_from_openvrpaths() {
      handle_t token {platf::retrieve_users_token(false)};
      if (!token) {
        return {};
      }
      PWSTR base = nullptr;
      if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, token.h, &base)) || !base) {
        if (base) {
          CoTaskMemFree(base);
        }
        return {};
      }
      const std::filesystem::path file = std::filesystem::path {base} / L"openvr" / L"openvrpaths.vrpath";
      CoTaskMemFree(base);
      std::error_code ec;
      if (!std::filesystem::is_regular_file(file, ec) || std::filesystem::file_size(file, ec) > (1u << 20)) {
        return {};
      }
      FILE *f = _wfopen(file.c_str(), L"rb");
      if (!f) {
        return {};
      }
      std::string text;
      char chunk[4096];
      size_t n = 0;
      while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
        text.append(chunk, n);
      }
      std::fclose(f);
      const auto j = nlohmann::json::parse(text, nullptr, false);
      if (j.is_discarded() || !j.is_object() || !j.contains("runtime") || !j["runtime"].is_array() || j["runtime"].empty() || !j["runtime"][0].is_string()) {
        return {};
      }
      const auto runtime = utf_utils::from_utf8(j["runtime"][0].get<std::string>());
      return (std::filesystem::path {runtime} / L"bin" / L"win64" / L"vrserver.exe").wstring();
    }

    /// 路徑只印 SteamVR 之後的尾端（前面可能含使用者名稱）
    std::string path_tail(const std::wstring &p) {
      const auto s = utf_utils::to_utf8(p);
      const auto pos = s.find("SteamVR");
      return pos == std::string::npos ? "<other>"s : "..\\" + s.substr(pos);
    }

    /// T4 的合成 tracking：2×Hz（180 Hz）、flags HMD|PRESENCE、target = now + predict；motion = still／yaw30／sine
    class t4_synth_t {
    public:
      t4_synth_t(std::string motion, uint16_t space_epoch):
          motion_(std::move(motion)),
          space_epoch_(space_epoch) {
        thread_ = std::thread([this]() {
          run();
        });
      }

      ~t4_synth_t() {
        stop_.store(true);
        if (thread_.joinable()) {
          thread_.join();
        }
      }

      t4_synth_t(const t4_synth_t &) = delete;
      t4_synth_t &operator=(const t4_synth_t &) = delete;

      uint64_t published() const {
        return published_.load();
      }

    private:
      void run() {
        platf::set_thread_name("vr_selftest_t4_synth");
        handle_t timer {CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)};
        const int64_t f = platf::qpc_frequency();
        const int64_t t0 = platf::qpc_counter();
        uint32_t id = 1;
        constexpr double k_pi = 3.14159265358979323846;
        while (!stop_.load()) {
          vr::bridge::tracked_sample_t s;
          const int64_t now = platf::qpc_counter();
          const int64_t predict = f / 50;  // 20 ms
          const double t = (double) (now - t0) / (double) f;
          double yaw = 0.0, yaw_rate = 0.0;
          if (motion_ == "yaw30") {
            yaw = std::fmod(30.0 * t, 360.0) * k_pi / 180.0;
            yaw_rate = 30.0 * k_pi / 180.0;
          } else if (motion_ == "sine") {
            const double w = 2.0 * k_pi * 0.25;
            yaw = 20.0 * k_pi / 180.0 * std::sin(w * t);
            yaw_rate = 20.0 * k_pi / 180.0 * w * std::cos(w * t);
          }
          s.sample_id = id++;
          s.space_epoch = space_epoch_;
          // VIPLE_VR_TRK_HMD | VIPLE_VR_TRK_PRESENCE：沒有 presence 時 pauseCompositorOnStandby 可能讓 compositor 停止 Present
          s.flags = 0x01 | 0x10;
          s.sample_time_ns = static_cast<uint64_t>(platf::qpc_ticks_to_ns(now));
          s.arrival_qpc = now;
          s.target_server_qpc = now + predict;
          s.predict_ns = static_cast<uint32_t>(platf::qpc_ticks_to_ns(predict));
          s.pose[0].pos[1] = 1.6f;
          s.pose[0].rot[1] = static_cast<float>(std::sin(yaw / 2.0));
          s.pose[0].rot[3] = static_cast<float>(std::cos(yaw / 2.0));
          s.pose[0].ang_vel[1] = static_cast<float>(yaw_rate);
          bridge_view::publish(s);
          published_.fetch_add(1);
          LARGE_INTEGER due;
          due.QuadPart = -55555;  // 5.56 ms（180 Hz）
          if (timer && SetWaitableTimer(timer.h, &due, 0, nullptr, nullptr, FALSE)) {
            WaitForSingleObject(timer.h, 100);
          } else {
            std::this_thread::sleep_for(5ms);
          }
        }
      }

      std::string motion_;
      uint16_t space_epoch_;
      std::atomic<bool> stop_ {false};
      std::atomic<uint64_t> published_ {0};
      std::thread thread_;
    };

    /// T4 的角落讀回（每 10 幀一次；在擷取執行緒上呼叫，與 capture() 用同一個 immediate context，順序天然成立）
    struct t4_readback_t {
      platf::dxgi::display_vr_t &disp;
      uint32_t eye_w;
      uint32_t eye_h;
      platf::dxgi::texture2d_t staging;
      uint32_t rw = 0, rh = 0;
      uint64_t frames = 0;
      uint64_t compared = 0;
      uint64_t mismatch = 0;
      uint64_t decode_fail = 0;  ///< 圖案沒通過 CRC（scene app 還沒畫、SteamVR 的載入畫面等）
      uint64_t eye_disagree = 0;  ///< 兩眼解出來的 counter 或姿態不同
      uint64_t echo_matched = 0;
      double max_angle = 0.0;
      std::set<uint16_t> counters;
      std::string first_mismatch;
      int dumps = 0;
      int dumps_ok = 0;
      std::string dump_dir;  ///< 非空才寫診斷用的 PGM（兩眼角落的亮度）

      /// 診斷：把讀回的角落區域（兩眼並排）寫成 PGM；只寫 SYSTEM／管理員的 config\steamvr\selftest
      void dump(const uint8_t *base, UINT pitch, const char *what) {
        if (dump_dir.empty()) {
          return;
        }
        ++dumps;
        std::error_code ec;
        std::filesystem::create_directories(dump_dir, ec);
        const auto file = std::filesystem::path {dump_dir} / std::format("t4-{}-{}-{}.pgm", what, frames, dumps);
        FILE *f = _wfopen(file.c_str(), L"wb");
        if (!f) {
          return;
        }
        std::fprintf(f, "P5\n%u %u\n255\n", 2 * rw, rh);
        std::vector<uint8_t> row(2 * rw);
        for (uint32_t y = 0; y < rh; ++y) {
          const uint8_t *src = base + (size_t) y * pitch;
          for (uint32_t x = 0; x < 2 * rw; ++x) {
            row[x] = (uint8_t) (((uint32_t) src[x * 4] + src[x * 4 + 1] + src[x * 4 + 2]) / 3);
          }
          std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
      }

      bool init() {
        rw = vr_scene_pattern::region_w(eye_w);
        rh = vr_scene_pattern::region_h(eye_h);
        D3D11_TEXTURE2D_DESC d {};
        d.Width = 2 * rw;
        d.Height = rh;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D *t = nullptr;
        if (FAILED(disp.device->CreateTexture2D(&d, nullptr, &t))) {
          return false;
        }
        staging.reset(t);
        return true;
      }

      void on_frame(platf::img_t &img) {
        auto &d3d = static_cast<platf::dxgi::img_d3d_t &>(img);
        if (d3d.blank || !img.vr || !d3d.capture_texture) {
          return;
        }
        if (++frames % 10 != 0) {
          return;
        }
        for (uint32_t e = 0; e < 2; ++e) {
          D3D11_BOX box {e * eye_w, 0, 0, e * eye_w + rw, rh, 1};
          disp.device_ctx->CopySubresourceRegion(staging.get(), 0, e * rw, 0, 0, d3d.capture_texture.get(), 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE m {};
        if (FAILED(disp.device_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m))) {
          return;
        }
        uint16_t cnt[2] = {};
        uint8_t q48[2][6] = {};
        bool ok[2] = {};
        const auto *base = static_cast<const uint8_t *>(m.pData);
        for (uint32_t e = 0; e < 2; ++e) {
          auto luma = [&](uint32_t x, uint32_t y) -> uint32_t {
            if (x >= rw || y >= rh) {
              return 0u;
            }
            const uint8_t *px = base + (size_t) y * m.RowPitch + (size_t) (e * rw + x) * 4;
            return ((uint32_t) px[0] + px[1] + px[2]) / 3;
          };
          ok[e] = vr_scene_pattern::decode(eye_w, eye_h, luma, cnt[e], q48[e]);
        }
        // 診斷：開頭一張、播放中（300 幀之後）再兩張失敗的；第一張成功的
        if ((!ok[0] || !ok[1]) && (dumps == 0 || (frames > 300 && dumps < 3))) {
          dump(base, m.RowPitch, "fail");
        } else if (ok[0] && ok[1] && dumps_ok < 1) {
          ++dumps_ok;
          dump(base, m.RowPitch, "ok");
        }
        disp.device_ctx->Unmap(staging.get(), 0);
        if (!ok[0] || !ok[1]) {
          ++decode_fail;
          return;
        }
        if (cnt[0] != cnt[1] || std::memcmp(q48[0], q48[1], 6) != 0) {
          ++eye_disagree;
        }
        float q[4];
        VipleVrUnpackQuat48(q48[0], q);
        const auto &meta = *img.vr;
        // |dot| → 角距離（q 與 −q 是同一個旋轉）
        double dot = 0.0, n1 = 0.0, n2 = 0.0;
        for (int i = 0; i < 4; ++i) {
          dot += (double) q[i] * meta.rot[i];
          n1 += (double) q[i] * q[i];
          n2 += (double) meta.rot[i] * meta.rot[i];
        }
        double c = n1 > 0 && n2 > 0 ? std::fabs(dot) / std::sqrt(n1 * n2) : 0.0;
        c = std::min(c, 1.0);
        const double ang = 2.0 * std::acos(c) * 57.29577951308232;
        ++compared;
        counters.insert(cnt[0]);
        if (meta.flags & VRIPC_FRM_ECHO_MATCHED) {
          ++echo_matched;
        }
        max_angle = std::max(max_angle, ang);
        if (!(ang < 0.01)) {
          ++mismatch;
          if (first_mismatch.empty()) {
            first_mismatch = std::format("frame={} counter={} angDeg={:.4f} decoded=({:.4f},{:.4f},{:.4f},{:.4f}) desc=({:.4f},{:.4f},{:.4f},{:.4f})", meta.frame_id, cnt[0], ang, q[0], q[1], q[2], q[3], meta.rot[0], meta.rot[1], meta.rot[2], meta.rot[3]);
          }
        }
      }
    };
  }  // namespace

  void run_t1(reporter_t &r, const request_t &, const std::atomic<bool> &stop) {
    // (a) display_vr_t 探測形態：不需要 IPC；此時沒有 session config → adapter 走 K22 候選規則
    {
      ::video::config_t cfg {};
      const auto shape = t1_shape();
      cfg.width = shape.packed_w;
      cfg.height = shape.packed_h;
      cfg.framerate = shape.hz;
      cfg.captureSource = 1;
      auto disp = std::make_shared<platf::dxgi::display_vr_t>();
      const int irc = disp->init(cfg);
      r.check("T1.init"sv, irc == 0, std::format("rc={} size={}x{}@{}", irc, cfg.width, cfg.height, cfg.framerate));
      if (irc == 0) {
        SS_HDR_METADATA md;
        std::memset(&md, 0xAB, sizeof(md));
        const bool hdr = disp->is_hdr();
        const bool has_md = disp->get_hdr_metadata(md);
        bool md_zero = true;
        for (size_t i = 0; i < sizeof(md); ++i) {
          md_zero = md_zero && reinterpret_cast<const uint8_t *>(&md)[i] == 0;
        }
        r.check("T1.hdr"sv, !hdr && !has_md && md_zero, std::format("isHdr={} hasMetadata={} metadataZeroed={}", hdr ? 1 : 0, has_md ? 1 : 0, md_zero ? 1 : 0));

        auto img = disp->alloc_img();
        const int drc = disp->dummy_img(img.get());
        const int crc = disp->complete_img(img.get(), false);
        D3D11_TEXTURE2D_DESC d {};
        auto *d3d = static_cast<platf::dxgi::img_d3d_t *>(img.get());
        if (d3d->capture_texture) {
          d3d->capture_texture->GetDesc(&d);
        }
        const bool img_ok = drc == 0 && crc == 0 && d.Width == (UINT) cfg.width && d.Height == (UINT) cfg.height && d.Format == DXGI_FORMAT_B8G8R8A8_UNORM && !d3d->dummy && d3d->fence_shared_handle && d3d->encoder_texture_handle;
        r.check("T1.img"sv, img_ok, std::format("dummyRc={} completeRc={} tex={}x{} fmt={} eventDriven={}", drc, crc, d.Width, d.Height, (uint32_t) d.Format, disp->is_event_driven() ? 1 : 0));
      }
    }
    if (stop.load()) {
      return;
    }

    // (b) VR 形狀的 encoder 探測（先以 VR config reset_display，IR 真的開起來）；結束時還原桌面探測狀態
    const auto before = desktop_probe_view_t::now();
    const auto t0 = std::chrono::steady_clock::now();
    const auto [rc, vr_ir] = vr_probe_once();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const auto after = desktop_probe_view_t::now();
    const auto shape = t1_shape();
    r.check("T1.probe"sv, rc == 0, std::format("rc={} ms={} shape={}x{}@{} irFrames={}", rc, ms, shape.packed_w, shape.packed_h, shape.hz, shape.ir_frames));
    r.check("T1.vr-intra-refresh"sv, vr_ir, std::format("vr_intra_refresh={} (expected 1 on NVENC; see [VIPLE-VR-ENC] intra refresh in sunshine.log)", vr_ir ? 1 : 0));
    r.check("T1.restore"sv, before == after, std::format("before=[{}] after=[{}]", before.str(), after.str()));
  }

  void run_t1b(reporter_t &r, const request_t &, const std::atomic<bool> &stop) {
    std::string err;
    const auto before = fetch_serverinfo_xml(err);
    if (!before) {
      r.check("T1b"sv, false, "serverinfo before: " + err);
      return;
    }
    if (stop.load()) {
      return;
    }
    const auto [rc, vr_ir] = vr_probe_once();
    const auto after = fetch_serverinfo_xml(err);
    if (!after) {
      r.check("T1b"sv, false, "serverinfo after: " + err);
      return;
    }
    // 目前 /serverinfo 沒有時間欄位（VR 探測前後 state／currentgame 不變），整份逐字比對
    const bool same = *before == *after;
    r.check("T1b.serverinfo"sv, same, same ? std::format("identical bytes={} probeRc={} vrIntraRefresh={}", before->size(), rc, vr_ir ? 1 : 0) : first_diff(*before, *after));
    // VR /launch 失敗出口（vr_negotiate 失敗、強制加密拒絕、execute 失敗）後的比對：pcvr 的 /launch 在 S1-11
    r.not_run("T1b.launch-exits"sv, "V5"sv, "VR /launch failure exits need vr_pcvr=enabled (S1-11); the probe scope API is in place"sv);
  }

  bool supported() {
    return true;
  }

  void run_t0(reporter_t &r, const request_t &, const std::atomic<bool> &stop) {
    DWORD self_session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &self_session);
    const auto user = get_console_user();
    const bool system = platf::is_running_as_system();

    r.info("T0.env"sv, std::format("selfSession={} consoleSession={} system={}", self_session, user.console_session == 0xFFFFFFFF ? -1 : static_cast<int64_t>(user.console_session), system ? 1 : 0));
    r.check("T0.system"sv, system, system ? "server runs as LocalSystem"sv : "server is not LocalSystem: the VR pipe cannot be created (expected fail-closed in console mode)"sv);
    r.check("T0.session"sv, user.console_session != 0xFFFFFFFF && self_session == user.console_session, std::format("self={} console={} (sec-m19: mismatch disables PCVR)", self_session, user.console_session == 0xFFFFFFFF ? -1 : static_cast<int64_t>(user.console_session)));
    if (user.present) {
      // UAC 關閉時 elevationType=full（殘餘風險 R-d：主控台使用者的程式都是提升的）
      r.info("T0.console-user"sv, std::format("present=1 rid={} elevation={} il={} logonSid={}", user.user_rid, elevation_name(user.elevation), il_name(user.il_rid), user.logon_sid.empty() ? "missing"sv : "present"sv));
    } else {
      r.info("T0.console-user"sv, "present=0 (nobody logged on at the console; the VR pipe keeps a SYSTEM-only DACL)"sv);
    }

    // DXGI adapter 清單（K22 的規則在 V5；這裡只列出，並標出 T2 會用哪一張）
    const auto adapters = list_adapters();
    const auto picked = pick_adapter(adapters);
    for (const auto &a : adapters) {
      r.info("T0.adapter"sv, std::format("index={} vendor=0x{:04x} vramMB={} flags=0x{:x} luid={} picked={} desc=\"{}\"", a.index, a.vendor, a.vram / (1024 * 1024), a.flags, luid_str(a.luid), picked && picked->index == a.index ? 1 : 0, a.desc));
    }
    if (stop.load()) {
      return;
    }

    // VR pipe（bridge）：O:SY、D:P(A;;GA;;;SY)(A;;0x12019b;;;<console logon SID>)、S:(ML;;NWNR;;;ME)（K3、§B.1）
    auto objs = bridge_view::objects();
    if (!objs.pipe) {
      const auto st = bridge_view::status();
      r.check("T0.vr-pipe"sv, false, std::format("bridge has no VR pipe (phase={} pipeReason={}; see the VIPLE-VR-IPC lines)", st.phase, st.pipe_reason));
    } else {
      const bool want_user_ace = user.present && !user.logon_sid.empty();
      sd_expect_t e {FILE_ALL_ACCESS, std::nullopt, 0, SECURITY_MANDATORY_MEDIUM_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP | SYSTEM_MANDATORY_LABEL_NO_READ_UP};
      if (want_user_ace) {
        e.second_who = who_e::logon_match;
        e.second_mask = 0x12019b;
      }
      const auto sd = read_sd(objs.pipe.h, user.logon_sid);
      const auto why = sd_mismatch(sd, e);
      r.check("T0.vr-pipe-sd"sv, why.empty(), describe(sd) + (want_user_ace ? " expectUserAce=1"s : " expectUserAce=0"s) + (why.empty() ? ""s : " mismatch=\"" + why + "\""));

      const std::wstring expect_name = std::format(L"\\VipleStreamVR-{}", self_session);
      const std::wstring name = pipe_name(objs.pipe.h);
      if (name.empty()) {
        r.info("T0.vr-pipe-name"sv, std::format("name query failed err={} (expect {})", GetLastError(), utf_utils::to_utf8(expect_name)));
      } else {
        r.check("T0.vr-pipe-name"sv, _wcsicmp(name.c_str(), expect_name.c_str()) == 0, std::format("name={} expect={}", utf_utils::to_utf8(name), utf_utils::to_utf8(expect_name)));
      }

      // SetSecurityInfo 來回（成功＝server 的 handle 帶 WRITE_DAC）：讀與原樣寫回都交給 bridge 在 pipe_mtx_ 內做。
      // selftest 自己拿複本「讀 → 寫回」會與 refresh_user() 換使用者 ACE 競態：夾在中間換了人的話，舊 DACL 被寫回、
      // 新使用者就一直被 ACCESS_DENIED（複本現在也只有 READ_CONTROL，寫不了）。
      const auto rt = bridge_view::security_roundtrip();
      if (!rt.pipe_exists) {
        r.check("T0.vr-pipe-setsecurity"sv, false, std::format("bridge has no VR pipe (pipeReason={})", rt.pipe_reason));
      } else if (rt.query_error != 0) {
        r.check("T0.vr-pipe-setsecurity"sv, false, std::format("GetSecurityInfo err={} (inside the bridge)", rt.query_error));
      } else {
        // 之後這一次讀取不持 bridge 的鎖：與來回前的位元組不同只當參考（合法的換 ACE 也會讓它不同），不列入判定
        const auto after = read_sd(objs.pipe.h, user.logon_sid);
        const bool unchanged = sd.err == ERROR_SUCCESS && after.err == ERROR_SUCCESS && after.dacl_bytes == sd.dacl_bytes;
        const bool pass = rt.roundtrip_attempted && rt.roundtrip_ok && rt.owner_is_system && rt.dacl_protected && after.err == ERROR_SUCCESS && after.dacl_protected;
        r.check("T0.vr-pipe-setsecurity"sv, pass, std::format("setErr={} ownerSY={} protected={} protectedAfter={} bridgeUserAce={} matchesConsole={} daclUnchangedSinceRead={}", rt.roundtrip_error, rt.owner_is_system ? 1 : 0, rt.dacl_protected ? 1 : 0, after.dacl_protected ? 1 : 0, rt.user_ace ? 1 : 0, rt.user_ace_matches_console ? 1 : 0, unchanged ? 1 : 0));
      }
    }

    // 本 generation 的 shm／event 只在有 driver 連線時存在；沒有就留給 T2（peer 連線期間檢查）
    if (!check_generation_objects(r, "T0.vr-obj"sv, user)) {
      r.info("T0.vr-obj"sv, "no live generation; shm/event descriptors are checked in T2 while the peer is connected"sv);
    }

    // admin pipe：O:SY、D:P(A;;GA;;;SY)(A;;0x12019b;;;BA)、S:(ML;;NWNR;;;HI)（§F.5）
    handle_t admin {platf::vr_admin::duplicate_admin_pipe_for_selftest()};
    if (!admin) {
      r.check("T0.admin-pipe-sd"sv, false, "admin pipe handle unavailable"sv);
    } else {
      check_sd(r, "T0.admin-pipe-sd"sv, admin.h, user, {FILE_ALL_ACCESS, who_e::ba, 0x12019b, SECURITY_MANDATORY_HIGH_RID, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP | SYSTEM_MANDATORY_LABEL_NO_READ_UP});
      const std::wstring name = pipe_name(admin.h);
      if (name.empty()) {
        r.info("T0.admin-pipe-name"sv, std::format("name query failed err={}", GetLastError()));
      } else {
        r.check("T0.admin-pipe-name"sv, _wcsicmp(name.c_str(), L"\\VipleStreamAdmin") == 0, std::format("name={}", utf_utils::to_utf8(name)));
      }
    }
  }

  void run_t2(reporter_t &r, const request_t &req, const std::atomic<bool> &stop) {
    const auto probe = probe_exe(req);
    if (auto why = probe_unusable(probe)) {
      r.check("T2"sv, false, *why);
      return;
    }
    // bridge 只放行 <install>\tools\vr_probe\vr_probe.exe（§B.1 第 5 步的例外綁安裝目錄；<install> 是 BU RX）
    const auto default_probe = install_dir() / L"tools" / L"vr_probe" / L"vr_probe.exe";
    if (_wcsicmp(probe.wstring().c_str(), default_probe.wstring().c_str()) != 0) {
      r.check("T2"sv, false, "T2 needs vr_probe at <install>\\tools\\vr_probe\\vr_probe.exe: the bridge admits only that image as the selftest peer (--probe-path applies to T6 only)"sv);
      return;
    }
    if (!platf::is_running_as_system()) {
      r.check("T2"sv, false, "server is not LocalSystem (the VR bridge only runs in the SYSTEM service)"sv);
      return;
    }
    const auto user = get_console_user();
    if (!user.present) {
      r.check("T2"sv, false, "no console user: vr_probe runs as the console user"sv);
      return;
    }
    if (process_running(L"vrserver.exe")) {
      r.check("T2"sv, false, "vrserver.exe is running: T2 needs SteamVR closed"sv);
      return;
    }
    // 使用者 ACE 平時靠輪詢加上；先要求 bridge 立刻重查，最多等 3 s 離開 no-user
    bridge_view::refresh_user();
    wait_until(3000ms, stop, []() {
      const auto s = bridge_view::status();
      return s.phase != "no-user"sv && s.phase != "no-pipe"sv && s.phase != "stopped"sv;
    });
    const auto st = bridge_view::status();
    if (st.phase == "stopped"sv || st.phase == "no-pipe"sv || st.phase == "no-user"sv) {
      r.check("T2"sv, false, std::format("bridge is not accepting connections (phase={} pipeReason={})", st.phase, st.pipe_reason));
      return;
    }
    if (st.connected) {
      r.check("T2"sv, false, "a driver is connected to the VR pipe: T2 needs it idle"sv);
      return;
    }
    const auto adapter = pick_adapter(list_adapters());
    if (!adapter) {
      r.check("T2"sv, false, "no hardware DXGI adapter for the ring textures"sv);
      return;
    }

    t2_ctx_t c {r, stop, probe, user, make_config(*adapter, 1, 1)};
    r.info("T2.setup"sv, std::format("adapter={} luid={} eye=1024x1024 hz=90 phase={}", adapter->index, luid_str(adapter->luid), st.phase));
    auto restore = util::fail_guard([]() {
      bridge_view::revoke_peer();
      bridge_view::clear_config();
    });
    if (!bridge_view::set_config(c.cfg)) {
      r.check("T2"sv, false, "bridge rejected the selftest session config"sv);
      return;
    }

    // 順序：成功的情境在前；REJECT 類會讓 server 的退避 streak 疊高（1→2→4 s），放在後面，以 recover 歸零；
    // 限速（streak 疊到 10、約 70 s）放最後，再 recover 一次
    using scenario_fn = void (*)(t2_ctx_t &);
    const scenario_fn scenarios[] = {
      t2_second_instance,
      t2_handshake,
      t2_consume,
      t2_consume_noflush,
      t2_kill,
      t2_reconnect,
      t2_fence_max,
      t2_stop_signal,
      t2_ownership,
      t2_never_read,
      t2_dual_writer,
      t2_escalate,
      t2_live,
    };
    for (auto fn : scenarios) {
      if (stop.load()) {
        return;
      }
      fn(c);
    }
    if (!stop.load()) {
      t2_expect_reject(c, "T2.abi-mismatch"sv, "--mode ipcpeer --seconds 15 --abi 2 --hello-size 128", true, VRIPC_REJ_ABI_MISMATCH, "ABI_MISMATCH"sv);
    }
    if (!stop.load()) {
      t2_expect_reject(c, "T2.bad-hello-size"sv, "--mode ipcpeer --seconds 15 --hello-size 100", true, VRIPC_REJ_BAD_MESSAGE, "BAD_MESSAGE"sv);
    }
    if (!stop.load()) {
      t2_impostor(c);
    }
    if (!stop.load()) {
      t2_recover(c, "T2.recover"sv);
    }
    if (!stop.load()) {
      t2_rate_limit(c);
    }
    if (!stop.load()) {
      t2_recover(c, "T2.recover-after-rate-limit"sv);
    }
    if (stop.load()) {
      return;
    }
    // §F.5 T2 列出、但還沒驗得了的情境：明列 NOT-RUN（計入 notRun=），rc=0 不代表它們驗過。
    // T2.live 已在 V3 實作（上面的 t2_live）；gpu-hold 需要 vr_probe 的 GPU hold（ipcpeer 目前回 not-implemented），不在 V3 的驗收內。
    r.not_run("T2.gpu-hold"sv, "V4"sv, "U31 implicit sync (--peer-gpu-hold-ms 50 -> server copy latency): vr_probe ipcpeer does not implement the GPU hold yet"sv);
  }

  void run_t6(reporter_t &r, const request_t &req, const std::atomic<bool> &stop) {
    t6_qpc(r);
    t6_frame_desc(r);
    if (stop.load()) {
      return;
    }
    const auto probe = probe_exe(req);
    if (auto why = probe_unusable(probe)) {
      r.check("T6.probe-unit"sv, false, *why);
      r.check("T6.abi-table"sv, false, *why);
      return;
    }
    t6_probe(r, probe, stop);
  }
  /**
   * T4（M1b V4、§F.5）：只支援 `--manual-steamvr`（T3 的 SteamVR 編排在 V5）。
   * 不註冊、不套 guard、不啟停 SteamVR（那些由本機操作手冊以使用者身分做，S3-10）；這裡只：
   *   設「合法 driver 宿主」映像（執行中的 vrserver，或使用者 openvrpaths 的 runtime）→ 設 session config → arm →
   *   合成 tracking（180 Hz，HMD|PRESENCE）→ 等真的 driver（identity=vrserver）READY 最多 120 s →
   *   以使用者身分啟動 vr_probe（--probe）→ selftest consumer（display_vr_t live）跑 hold 秒；scene 時每 10 幀讀回
   *   兩眼左上角的位元圖案，與 descriptor 的 renderPose 比對（角誤差 < 0.01°）→ disarm、清 config、清宿主映像。
   */
  void run_t4(reporter_t &r, const request_t &req, const std::atomic<bool> &stop) {
    if (!req.manual_steamvr) {
      r.check("T4"sv, false, "V4 runs T4 only with --manual-steamvr (the T3 SteamVR orchestration is V5)"sv);
      return;
    }
    if (!platf::is_running_as_system()) {
      r.check("T4"sv, false, "server is not LocalSystem (the VR bridge only runs in the SYSTEM service)"sv);
      return;
    }
    const auto user = get_console_user();
    if (!user.present) {
      r.check("T4"sv, false, "no console user: SteamVR and vr_probe run as the console user"sv);
      return;
    }
    std::filesystem::path probe;
    if (req.probe != "none"sv) {
      probe = probe_exe(req);
      if (auto why = probe_unusable(probe)) {
        r.check("T4"sv, false, *why);
        return;
      }
    }
    bridge_view::refresh_user();
    wait_until(3000ms, stop, []() {
      const auto s = bridge_view::status();
      return s.phase != "no-user"sv && s.phase != "no-pipe"sv && s.phase != "stopped"sv;
    });
    const auto st0 = bridge_view::status();
    if (st0.phase == "stopped"sv || st0.phase == "no-pipe"sv || st0.phase == "no-user"sv) {
      r.check("T4"sv, false, std::format("bridge is not accepting connections (phase={} pipeReason={})", st0.phase, st0.pipe_reason));
      return;
    }
    if (st0.connected && st0.peer_is_selftest) {
      r.check("T4"sv, false, "a selftest peer is connected to the VR pipe"sv);
      return;
    }
    const auto adapter = pick_adapter(list_adapters());
    if (!adapter) {
      r.check("T4"sv, false, "no hardware DXGI adapter"sv);
      return;
    }

    // 「合法 driver 宿主」映像（§B.1 第 5 步）：先看執行中的 vrserver，沒有就讀使用者的 openvrpaths
    std::wstring host = running_vrserver_image();
    std::string host_src = "running";
    if (host.empty()) {
      host = vrserver_image_from_openvrpaths();
      host_src = "openvrpaths";
    }
    if (host.empty()) {
      r.check("T4"sv, false, "cannot determine the vrserver.exe path (SteamVR not running and openvrpaths.vrpath has no runtime)"sv);
      return;
    }
    const bool steamvr_running = process_running(L"vrserver.exe");
    vr::bridge::set_expected_driver_host_image(host);

    const auto cfg = make_config_t4(*adapter);
    const int hold = req.hold_sec > 0 ? req.hold_sec : 60;
    r.info("T4.setup"sv, std::format("adapter={} luid={} eye={}x{} hz={} motion={} probe={} holdSec={} hostImage={} source={} steamvrRunning={} phase={} connected={}", adapter->index,
                                     luid_str(adapter->luid), cfg.eye_width, cfg.eye_height, cfg.refresh_mhz / 1000, req.motion, req.probe, hold, path_tail(host), host_src,
                                     steamvr_running ? 1 : 0, st0.phase, st0.connected ? 1 : 0));
    auto restore = util::fail_guard([]() {
      vr::bridge::set_armed(false);
      bridge_view::clear_config();
      vr::bridge::set_expected_driver_host_image(L"");
    });
    if (!bridge_view::set_config(cfg)) {
      r.check("T4"sv, false, "bridge rejected the T4 session config"sv);
      return;
    }
    vr::bridge::set_armed(true);
    t4_synth_t synth(req.motion, cfg.space_epoch);

    // 等真的 driver（不是 selftest peer）READY：操作手冊在這段時間以使用者身分啟動 SteamVR
    const auto t_wait0 = steady::now();
    auto last_progress = t_wait0;
    const int64_t ready_ms = wait_until(120000ms, stop, [&]() {
      const auto s = bridge_view::status();
      const auto now = steady::now();
      if (host_src != "running"sv) {
        // vrserver 啟動了但路徑與 openvrpaths 推的不同：改用實際的（下一次握手生效）
        const auto running = running_vrserver_image();
        if (!running.empty()) {
          if (_wcsicmp(running.c_str(), host.c_str()) != 0) {
            host = running;
            vr::bridge::set_expected_driver_host_image(host);
          }
          host_src = "running";
        }
      }
      if (now - last_progress >= 10s) {
        last_progress = now;
        const auto c = bridge_view::counters();
        r.info("T4.wait"sv, std::format("sec={} phase={} connected={} ready={} peer={} rejects={} lastIdentity={} vrserver={}", std::chrono::duration_cast<std::chrono::seconds>(now - t_wait0).count(), s.phase,
                                        s.connected ? 1 : 0, s.ready ? 1 : 0, s.peer_is_selftest ? 1 : 0, bridge_view::rejects_total(c), c.last_identity, process_running(L"vrserver.exe") ? 1 : 0));
      }
      return s.connected && s.ready && !s.peer_is_selftest;
    },
                                        50ms);
    if (ready_ms < 0) {
      if (!stop.load()) {
        const auto s = bridge_view::status();
        const auto c = bridge_view::counters();
        r.check("T4.handshake"sv, false, std::format("the SteamVR driver never became READY within 120 s (phase={} connected={} ready={} rejects={} identityRejects={} vrserver={})", s.phase, s.connected ? 1 : 0,
                                                     s.ready ? 1 : 0, bridge_view::rejects_total(c), bridge_view::rejects(c, VRIPC_REJ_IDENTITY), process_running(L"vrserver.exe") ? 1 : 0));
      }
      return;
    }
    {
      const auto s = vr::bridge::status();
      const auto c = bridge_view::counters();
      const bool pass = !s.peer_is_selftest && (s.driver_caps & VRIPC_DCAP_PEER) == 0 && s.driver_iface == "IVRDriverDirectModeComponent_009" && c.last_identity == 1;
      r.check("T4.handshake"sv, pass, std::format("gen={} driver={}.{}.{} caps=0x{:x} iface={} identity={} waitMs={} hostSource={}", s.generation, s.driver_version_packed >> 24, (s.driver_version_packed >> 16) & 0xFF,
                                                  s.driver_version_packed & 0xFFFF, s.driver_caps, s.driver_iface, c.last_identity == 1 ? "vrserver"sv : "other"sv, ready_ms, host_src));
    }

    // HMD Activate（act_* 有效）最多 30 s
    const int64_t act_ms = wait_until(30000ms, stop, []() {
      const auto s = vr::bridge::status();
      return s.act_valid && s.act_refresh_mhz != 0;
    });
    {
      const auto s = vr::bridge::status();
      const bool pass = act_ms >= 0 && s.act_refresh_mhz == cfg.refresh_mhz && s.act_eye_w == cfg.eye_width && s.act_eye_h == cfg.eye_height;
      r.check("T4.hmd-activated"sv, pass, std::format("ms={} actHz={} actEye={}x{} hmdAdded={} driverState={} otherHmd={} degraded=0x{:x}", act_ms, s.act_refresh_mhz / 1000, s.act_eye_w, s.act_eye_h,
                                                      s.hmd_added ? 1 : 0, s.driver_state, s.other_hmd, s.degraded_code));
      if (!pass) {
        return;
      }
    }

    // vr_probe（主控台使用者身分、kill-on-close job）
    std::unique_ptr<probe_proc_t> p;
    if (!probe.empty()) {
      std::string args = std::format("--mode {} --seconds {}", req.probe, req.probe == "whoami"sv ? 1 : hold + 30);
      if (req.reset_seated) {
        args += " --reset-seated";
      }
      p = std::make_unique<probe_proc_t>(probe, args);
      if (!p->ok()) {
        r.check("T4.probe"sv, false, "vr_probe launch failed: " + p->error());
        return;
      }
    }
    auto probe_cleanup = util::fail_guard([&]() {
      if (p) {
        p->terminate();
      }
    });

    // HMD_PRESENTING（第一次合成並發布）最多 30 s
    const int64_t pres_ms = wait_until(30000ms, stop, []() {
      return vr::bridge::status().hmd_presenting;
    });
    r.check("T4.presenting"sv, pres_ms >= 0, std::format("ms={} driverState={}", pres_ms, vr::bridge::status().driver_state));
    if (pres_ms < 0 || stop.load()) {
      if (p) {
        relay(r, "T4"sv, *p);
      }
      return;
    }

    // selftest consumer：display_vr_t live、影像池 3 張；scene 時做角落讀回
    ::video::config_t vcfg {};
    vcfg.width = (int) cfg.packed_width;
    vcfg.height = (int) cfg.packed_height;
    vcfg.framerate = (int) (cfg.refresh_mhz / 1000);
    vcfg.captureSource = 1;
    auto disp = std::make_shared<platf::dxgi::display_vr_t>();
    if (disp->init(vcfg)) {
      r.check("T4.consume"sv, false, "display_vr_t init failed (see [VIPLE-VR-CAP] in sunshine.log)"sv);
      return;
    }
    t4_readback_t rb {*disp, cfg.eye_width, cfg.eye_height};
    rb.dump_dir = (install_dir() / L"config" / L"steamvr" / L"selftest").string();
    const bool do_readback = req.probe == "scene"sv;
    if (do_readback && !rb.init()) {
      r.check("T4.readback"sv, false, "staging texture creation failed"sv);
      return;
    }
    std::vector<std::shared_ptr<platf::img_t>> pool(3);
    uint64_t pushed = 0;
    const auto t0 = steady::now();
    auto last_line = t0;
    auto pull = [&](std::shared_ptr<platf::img_t> &out) -> bool {
      out.reset();
      while (!stop.load()) {
        for (auto &slot : pool) {
          if (!slot) {
            slot = disp->alloc_img();
          }
          if (slot.use_count() == 1) {
            out = slot;
            out->frame_timestamp.reset();
            out->vr.reset();
            return true;
          }
        }
        std::this_thread::sleep_for(1ms);
      }
      return false;
    };
    auto push = [&](std::shared_ptr<platf::img_t> &&img, bool captured) -> bool {
      if (captured && img) {
        ++pushed;
        if (do_readback) {
          rb.on_frame(*img);
        }
      }
      img.reset();
      const auto now = steady::now();
      if (now - last_line >= 10s) {
        last_line = now;
        r.info("T4.progress"sv, std::format("sec={} pushed={} compared={} mismatch={} decodeFail={} synth={}", std::chrono::duration_cast<std::chrono::seconds>(now - t0).count(), pushed, rb.compared, rb.mismatch,
                                            rb.decode_fail, synth.published()));
      }
      return !stop.load() && now - t0 < std::chrono::seconds(hold);
    };
    bool cursor = false;
    const auto status = disp->capture(push, pull, &cursor);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - t0).count();
    if (stop.load()) {
      return;
    }
    const auto s = disp->stats();
    const uint64_t seen = s.last_frame_id >= s.first_frame_id && s.first_frame_id != 0 ? s.last_frame_id - s.first_frame_id + 1 : 0;
    const double consumption = seen ? (double) s.copied / (double) seen : 0.0;
    r.check("T4.consume"sv, status == platf::capture_e::ok && s.copied > 0 && s.invalid == 0 && s.fence_lost == 0,
            std::format("ms={} status={} gen={} copied={} seen={} consumption={:.4f} skipped={} fenceTimeout={} invalid={} torn={} fenceLost={} black={} evtToPushMs p50={:.3f} p95={:.3f} presentToPushMs p50={:.3f} p95={:.3f} synth={}",
                        elapsed_ms, (int) status, s.generation, s.copied, seen, consumption, s.skipped, s.fence_timeout, s.invalid, s.torn, s.fence_lost, s.black, pct(s.evt_to_push_ms, 0.5), pct(s.evt_to_push_ms, 0.95),
                        pct(s.present_to_push_ms, 0.5), pct(s.present_to_push_ms, 0.95), synth.published()));
    if (do_readback) {
      const bool pass = rb.compared >= 20 && rb.mismatch == 0 && rb.eye_disagree == 0;
      r.check("T4.readback"sv, pass, std::format("compared={} mismatch={} eyeDisagree={} decodeFail={} distinctCounters={} echoMatched={} maxAngleDeg={:.5f}{}", rb.compared, rb.mismatch, rb.eye_disagree, rb.decode_fail,
                                                 rb.counters.size(), rb.echo_matched, rb.max_angle, rb.first_mismatch.empty() ? ""s : " first=[" + rb.first_mismatch + "]"));
    }
    disp.reset();
    const auto sd = vr::bridge::status();
    r.info("T4.driver"sv, std::format("driverState={} presenting={} otherHmd={} otherSystem={} degraded=0x{:x}", sd.driver_state, sd.hmd_presenting ? 1 : 0, sd.other_hmd, sd.other_hmd_system, sd.degraded_code));

    if (p) {
      const auto w = p->wait(40000ms, stop);
      const auto code = p->exit_code();
      const auto summary = p->last(req.probe, "summary"sv);
      r.check("T4.probe"sv, summary.has_value() && code && *code == 0, std::format("mode={} wait={} {}", req.probe, (int) w, peer_diag(*p)));
      relay(r, "T4"sv, *p);
    }
  }
}  // namespace vr::selftest::platform
