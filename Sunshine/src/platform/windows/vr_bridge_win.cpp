/**
 * @file src/platform/windows/vr_bridge_win.cpp
 * @brief VipleStream 2.0 §VR M1b（S1-07）：`vr_bridge` 的 Windows 實作——pipe server、不具名 shm 與 event、
 *        server 建立的 ring 貼圖與 fence、HELLO→WELCOME→TEXTURES→READY 握手、client 驗證、tracking 寫入端。
 *
 * 權威設計：M1b 設計 §B.1（身分與權限）、§B.3（seqlock／SPSC）、§B.4（握手與逾時）、§B.5（狀態機）、
 * §B.7（fence 交接）、§B.8（不可信輸入驗證）；K2（方案 B：server 建立、DuplicateHandle 進 vrserver）、
 * K3（pipe SD）、K5（client 驗證）、K26（tracking 單一寫入臨界區）。
 *
 * 執行緒模型：
 *   - 一條 `vr_bridge` 執行緒擁有 pipe IO、generation 的建立與拆除、header／pacing 的寫入、SPSC ring 的消費。
 *     連線中以 2 ms 的高解析度 waitable timer 驅動；沒有連線時只在 pipe 重試、使用者輪詢與退避到期時醒來。
 *   - publish_tracking() 在呼叫端執行緒（ENet control、picoquic IO、selftest）上，以 trk_mtx_ 序列化。
 *   - 其他 API（set_*、status()）只在 mtx_ 內改「期望狀態」並喚醒 bridge 執行緒，實際動作都在 bridge 執行緒。
 *
 * 所有 pipe 寫入都是 overlapped、100 ms 逾時 → CancelIoEx → TEARDOWN；永遠不在 pipe 上 FlushFileBuffers（sec-M8）。
 * log 不印 handle 值、完整 SID（只印 RID）、完整 GUID。
 */
#ifndef NOMINMAX
  #define NOMINMAX
#endif

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// platform includes
#include <Windows.h>
#include <aclapi.h>
#include <d3d11_4.h>
#include <dxgi1_4.h>
#include <sddl.h>
#include <wtsapi32.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/utf_utils.h"
#include "src/vr/vr_bridge.h"

using namespace std::literals;

namespace vr::bridge {
  namespace {
    // ── 常數（§B.1、§B.4、§B.5）──────────────────────────────────────────────
    /// 使用者 ACE：FILE_GENERIC_READ|FILE_WRITE_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES（不含 FILE_APPEND_DATA =
    /// FILE_CREATE_PIPE_INSTANCE，否則使用者可以在我們的 pipe 上加掛 instance；scout E §2.4 實測）
    constexpr DWORD kUserPipeAccess = 0x0012019Bu;
    constexpr DWORD kPipeBufferBytes = 4096;
    constexpr DWORD kHelloTimeoutMs = 2000;
    constexpr int64_t kReadyTimeoutMs = 5000;
    constexpr DWORD kWriteTimeoutMs = 100;
    constexpr DWORD kCloseWaitMs = 200;
    constexpr int64_t kHeartbeatMs = 100;
    constexpr int64_t kDriverHungMs = 2000;
    constexpr int64_t kDriverFirstBeatMs = 3000;
    constexpr int64_t kPipeRetryMs = 30000;
    constexpr int64_t kUserPollNoUserMs = 5000;
    constexpr int64_t kUserPollMs = 30000;
    constexpr uint32_t kDriverLogPerSec = 20;
    constexpr uint32_t kHapticPerSecPerHand = 200;
    constexpr uint32_t kStateLogPerSec = 20;
    constexpr uint32_t kRingCorruptLimit = 16;
    constexpr uint32_t kInvalidStreakLimit = 30;
    constexpr uint32_t kDefaultRefreshMhz = 90000;
    constexpr size_t kRecentSamples = 2048;
    // 握手失敗的退避秒數：vr_bridge.h 的 reject_backoff_sec()（selftest T2.rate-limit 用同一份表驗）

    // ── 時間 ───────────────────────────────────────────────────────────────
    int64_t now_qpc() {
      return platf::qpc_counter();
    }

    int64_t qpf() {
      const int64_t f = platf::qpc_frequency();
      return f > 0 ? f : 10'000'000;
    }

    int64_t ms_to_qpc(int64_t ms) {
      return ms * qpf() / 1000;
    }

    double qpc_to_ms(int64_t ticks) {
      return (double) platf::qpc_ticks_to_ns(ticks) / 1e6;
    }

    /// 剩餘 tick 換成 WaitFor* 的毫秒（無條件進位，至少 1、最多 1000）
    DWORD ceil_ms(int64_t ticks) {
      if (ticks <= 0) {
        return 0;
      }
      const int64_t ms = (ticks * 1000 + qpf() - 1) / qpf();
      return (DWORD) std::clamp<int64_t>(ms, 1, 1000);
    }

    std::string hex32(uint32_t v) {
      return std::format("0x{:08X}", v);
    }

    std::string version_text(uint32_t packed) {
      return std::format("{}.{}.{}", (packed >> 24) & 0xFFu, (packed >> 16) & 0xFFu, packed & 0xFFFFu);
    }

    uint32_t server_version_packed() {
#if defined(PROJECT_VERSION_MAJOR) && defined(PROJECT_VERSION_MINOR) && defined(PROJECT_VERSION_PATCH)
      static const uint32_t v = pack_version((uint32_t) std::atoi(PROJECT_VERSION_MAJOR), (uint32_t) std::atoi(PROJECT_VERSION_MINOR), (uint32_t) std::atoi(PROJECT_VERSION_PATCH));
      return v;
#else
      return 0;
#endif
    }

    // ── RAII ───────────────────────────────────────────────────────────────
    class handle_t {
    public:
      handle_t() = default;

      explicit handle_t(HANDLE h):
          h_(h) {
      }

      ~handle_t() {
        reset();
      }

      handle_t(const handle_t &) = delete;
      handle_t &operator=(const handle_t &) = delete;

      handle_t(handle_t &&o) noexcept:
          h_(std::exchange(o.h_, nullptr)) {
      }

      handle_t &operator=(handle_t &&o) noexcept {
        if (this != &o) {
          reset();
          h_ = std::exchange(o.h_, nullptr);
        }
        return *this;
      }

      HANDLE get() const {
        return h_;
      }

      explicit operator bool() const {
        return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
      }

      void reset(HANDLE h = nullptr) {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
          CloseHandle(h_);
        }
        h_ = h;
      }

    private:
      HANDLE h_ = nullptr;
    };

    template<class T>
    class com_ptr {
    public:
      com_ptr() = default;

      ~com_ptr() {
        reset();
      }

      com_ptr(const com_ptr &) = delete;
      com_ptr &operator=(const com_ptr &) = delete;

      com_ptr(com_ptr &&o) noexcept:
          p_(std::exchange(o.p_, nullptr)) {
      }

      com_ptr &operator=(com_ptr &&o) noexcept {
        if (this != &o) {
          reset();
          p_ = std::exchange(o.p_, nullptr);
        }
        return *this;
      }

      T *get() const {
        return p_;
      }

      T *operator->() const {
        return p_;
      }

      explicit operator bool() const {
        return p_ != nullptr;
      }

      T **put() {
        reset();
        return &p_;
      }

      void **put_void() {
        reset();
        return reinterpret_cast<void **>(&p_);
      }

      void reset() {
        if (p_) {
          p_->Release();
          p_ = nullptr;
        }
      }

    private:
      T *p_ = nullptr;
    };

    class local_sd_t {
    public:
      ~local_sd_t() {
        if (p) {
          LocalFree(p);
        }
      }

      bool from_sddl(const wchar_t *sddl) {
        return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &p, nullptr) != FALSE;
      }

      PSECURITY_DESCRIPTOR p = nullptr;
    };

    // ── SID 與 token ───────────────────────────────────────────────────────
    using sid_t = std::vector<uint8_t>;

    sid_t copy_sid(PSID sid) {
      if (!sid || !IsValidSid(sid)) {
        return {};
      }
      const DWORD n = GetLengthSid(sid);
      sid_t v(n);
      if (!CopySid(n, v.data(), sid)) {
        return {};
      }
      return v;
    }

    PSID as_psid(const sid_t &s) {
      return s.empty() ? nullptr : const_cast<PSID>(static_cast<const void *>(s.data()));
    }

    bool sid_equal(const sid_t &a, const sid_t &b) {
      return !a.empty() && !b.empty() && EqualSid(as_psid(a), as_psid(b)) != FALSE;
    }

    bool sid_equal(PSID a, const sid_t &b) {
      return a && !b.empty() && IsValidSid(a) && EqualSid(a, as_psid(b)) != FALSE;
    }

    uint32_t sid_rid(PSID s) {
      if (!s || !IsValidSid(s)) {
        return 0;
      }
      const UCHAR n = *GetSidSubAuthorityCount(s);
      return n ? (uint32_t) *GetSidSubAuthority(s, n - 1) : 0;
    }

    /// S-1-5-5-X-Y（logon SID）
    bool is_logon_sid(PSID s) {
      if (!s || !IsValidSid(s)) {
        return false;
      }
      const SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
      const auto *auth = GetSidIdentifierAuthority(s);
      return std::memcmp(auth, &nt, sizeof nt) == 0 && *GetSidSubAuthorityCount(s) == 3 && *GetSidSubAuthority(s, 0) == 5;
    }

    sid_t well_known_sid(WELL_KNOWN_SID_TYPE type) {
      DWORD n = SECURITY_MAX_SID_SIZE;
      sid_t v(n);
      if (!CreateWellKnownSid(type, nullptr, v.data(), &n)) {
        return {};
      }
      v.resize(n);
      return v;
    }

    std::vector<uint8_t> token_info(HANDLE tok, TOKEN_INFORMATION_CLASS cls) {
      DWORD need = 0;
      GetTokenInformation(tok, cls, nullptr, 0, &need);
      if (need == 0 || need > 64 * 1024) {
        return {};
      }
      std::vector<uint8_t> buf(need);
      if (!GetTokenInformation(tok, cls, buf.data(), need, &need)) {
        return {};
      }
      return buf;
    }

    struct token_ident_t {
      bool ok = false;  ///< 使用者 SID 與 logon SID 都取得到
      sid_t user;
      sid_t logon;
      uint32_t integrity = 0;  ///< integrity level 的 RID（0x2000 = Medium）
      bool app_container = true;  ///< 查不到時當成 true（fail closed）
    };

    token_ident_t query_token(HANDLE tok) {
      token_ident_t r;
      if (auto u = token_info(tok, TokenUser); u.size() >= sizeof(TOKEN_USER)) {
        r.user = copy_sid(reinterpret_cast<TOKEN_USER *>(u.data())->User.Sid);
      }
      // logon SID：TokenLogonSid（Win8+）；取不到就掃 TokenGroups 找 SE_GROUP_LOGON_ID
      if (auto l = token_info(tok, TokenLogonSid); l.size() >= sizeof(TOKEN_GROUPS)) {
        auto *g = reinterpret_cast<TOKEN_GROUPS *>(l.data());
        if (g->GroupCount >= 1) {
          r.logon = copy_sid(g->Groups[0].Sid);
        }
      }
      if (r.logon.empty()) {
        if (auto gr = token_info(tok, TokenGroups); gr.size() >= sizeof(TOKEN_GROUPS)) {
          auto *g = reinterpret_cast<TOKEN_GROUPS *>(gr.data());
          for (DWORD i = 0; i < g->GroupCount; ++i) {
            if ((g->Groups[i].Attributes & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) {
              r.logon = copy_sid(g->Groups[i].Sid);
              break;
            }
          }
        }
      }
      if (auto il = token_info(tok, TokenIntegrityLevel); il.size() >= sizeof(TOKEN_MANDATORY_LABEL)) {
        r.integrity = sid_rid(reinterpret_cast<TOKEN_MANDATORY_LABEL *>(il.data())->Label.Sid);
      }
      DWORD ac = 1;
      DWORD got = 0;
      if (GetTokenInformation(tok, TokenIsAppContainer, &ac, sizeof ac, &got)) {
        r.app_container = ac != 0;
      }
      r.ok = !r.user.empty() && !r.logon.empty();
      return r;
    }

    struct user_t {
      sid_t user;
      sid_t logon;
    };

    /// 指定 session 目前登入的使用者（WTSQueryUserToken；需要 SeTcbPrivilege，SYSTEM 有）。沒人登入 → nullopt。
    std::optional<user_t> query_session_user(DWORD session) {
      HANDLE raw = nullptr;
      if (!WTSQueryUserToken(session, &raw)) {
        return std::nullopt;
      }
      handle_t tok(raw);
      auto id = query_token(tok.get());
      if (!id.ok) {
        return std::nullopt;
      }
      return user_t {std::move(id.user), std::move(id.logon)};
    }

    /**
     * @brief ImpersonateNamedPipeClient 的 RAII（sec-M11 的精神）：解構時 RevertToSelf，失敗就記 fatal 並
     *        std::terminate()——不能帶著別人的身分繼續執行。範圍內只做 OpenThreadToken。
     */
    class pipe_impersonation_t {
    public:
      explicit pipe_impersonation_t(HANDLE pipe) {
        ok_ = ImpersonateNamedPipeClient(pipe) != FALSE;
        if (!ok_) {
          err_ = GetLastError();
        }
      }

      ~pipe_impersonation_t() {
        if (ok_ && !RevertToSelf()) {
          BOOST_LOG(fatal) << "[VIPLE-VR-IPC] RevertToSelf failed after pipe impersonation: " << GetLastError();
          std::terminate();
        }
      }

      pipe_impersonation_t(const pipe_impersonation_t &) = delete;
      pipe_impersonation_t &operator=(const pipe_impersonation_t &) = delete;

      bool ok() const {
        return ok_;
      }

      DWORD error() const {
        return err_;
      }

    private:
      bool ok_ = false;
      DWORD err_ = 0;
    };

    // ── 路徑 ───────────────────────────────────────────────────────────────
    std::wstring normalize_path(std::wstring p) {
      for (auto &c : p) {
        if (c == L'/') {
          c = L'\\';
        }
      }
      if (p.starts_with(L"\\\\?\\")) {
        p.erase(0, 4);
      }
      return p;
    }

    /// 不分大小寫（ordinal）比對；任一邊為空一律不相等
    bool path_equal(const std::wstring &a, const std::wstring &b) {
      return !a.empty() && !b.empty() && CompareStringOrdinal(a.c_str(), (int) a.size(), b.c_str(), (int) b.size(), TRUE) == CSTR_EQUAL;
    }

    /// 行程映像（Win32 路徑）；只查字串，不開檔（K5：不對 Users 可寫的檔案做 CreateFileW）
    std::wstring process_image(HANDLE proc) {
      std::wstring buf(32768, L'\0');
      DWORD n = (DWORD) buf.size();
      if (!QueryFullProcessImageNameW(proc, 0, buf.data(), &n)) {
        return {};
      }
      buf.resize(n);
      return normalize_path(std::move(buf));
    }

    /// `<install>`（server 自己的模組所在目錄）
    std::wstring install_dir() {
      std::wstring buf(32768, L'\0');
      const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD) buf.size());
      if (n == 0 || n >= buf.size()) {
        return {};
      }
      buf.resize(n);
      const auto pos = buf.find_last_of(L"\\/");
      if (pos == std::wstring::npos) {
        return {};
      }
      buf.resize(pos);
      return normalize_path(std::move(buf));
    }

    std::string narrow(const std::wstring &w) {
      return w.empty() ? std::string {"<none>"} : utf_utils::to_utf8(w);
    }

    /// 不可信字串進 log 之前：只留可列印 ASCII，'[' 與其他字元換成 '?'（§B.8 第 8、10 條）
    std::string sanitize(const char *s, size_t max_len) {
      std::string out;
      out.reserve(max_len);
      for (size_t i = 0; i < max_len && s[i] != '\0'; ++i) {
        const auto c = (unsigned char) s[i];
        out.push_back((c >= 0x20 && c <= 0x7E && c != '[') ? (char) c : '?');
      }
      return out;
    }

    /**
     * @brief 對方可控制的路徑（連線行程的映像等）進 log／event 之前：與 sanitize() 同規則，另把空白與 '=' 也換成 '?'，
     *        上限 260 字元。
     * @details 同一使用者的任何 medium 行程都能把自己放在任意可寫目錄（Win32 檔名允許 '['、']'、'='、空白），
     *          在 SYSTEM 的 sunshine.log 偽造 `[VIPLE-VR-IPC] handshake … identity=vrserver` 或 rejected 行後段的
     *          `rej=`／`pid=` 欄位。'?' 不是合法的 Win32 檔名字元，所以替換過的位置一眼可辨；整個路徑也保證是
     *          單一個以空白分隔的欄位（§F.10 的分析腳本以空白切欄）。非 ASCII 的 UTF-8 位元組逐一變成 '?'（順便擋
     *          RTL override 之類的視覺偽造）。
     */
    std::string loggable_path(const std::wstring &w) {
      const std::string u8 = narrow(w);
      std::string out = sanitize(u8.c_str(), std::min<size_t>(u8.size(), 260));
      for (auto &c : out) {
        if (c == ' ' || c == '=') {
          c = '?';
        }
      }
      return out;
    }

    bool finite_pose(const vripc_pose_t &p) {
      for (float v : p.pos) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      for (float v : p.rot) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      for (float v : p.lin_vel) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      for (float v : p.ang_vel) {
        if (!std::isfinite(v)) {
          return false;
        }
      }
      return true;
    }

    /// period_q32 = 每個 vsync 的 QPC ticks × 2^32（整數：freq × 2^32 × 1000 / mHz，拆成商與餘數避免溢位）
    uint64_t period_q32_for(uint32_t refresh_mhz) {
      if (refresh_mhz == 0) {
        refresh_mhz = kDefaultRefreshMhz;
      }
      const uint64_t f = (uint64_t) std::min<int64_t>(qpf(), 0x7FFFFFFF);  // QPF 實際是 10 MHz；上限只為了 << 32 不溢位
      const uint64_t num = f << 32;
      return (num / refresh_mhz) * 1000u + ((num % refresh_mhz) * 1000u) / refresh_mhz;
    }

    const char *state_kind_name(uint32_t k) {
      switch (k) {
        case VRIPC_ST_HMD_ADDED:
          return "hmd-added";
        case VRIPC_ST_HMD_ADD_REJECTED:
          return "hmd-add-rejected";
        case VRIPC_ST_HMD_ACTIVATED:
          return "hmd-activated";
        case VRIPC_ST_HMD_PRESENTING:
          return "hmd-presenting";
        case VRIPC_ST_HMD_STANDBY:
          return "hmd-standby";
        case VRIPC_ST_DEVICE_LOST:
          return "device-lost";
        case VRIPC_ST_EXITING:
          return "exiting";
        case VRIPC_ST_IFACE_UNSUPPORTED:
          return "iface-unsupported";
        case VRIPC_ST_DEGRADED:
          return "degraded";
        default:
          return "unknown";
      }
    }

    /// 在指定 LUID 的 adapter 上建 D3D11 device（FL 11_1／11_0）
    HRESULT create_device_on_luid(const luid_t &luid, com_ptr<ID3D11Device> &dev, com_ptr<ID3D11DeviceContext> *ctx) {
      com_ptr<IDXGIFactory1> f1;
      HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), f1.put_void());
      if (FAILED(hr)) {
        return hr;
      }
      com_ptr<IDXGIFactory4> f4;
      hr = f1->QueryInterface(__uuidof(IDXGIFactory4), f4.put_void());
      if (FAILED(hr)) {
        return hr;
      }
      LUID l;
      l.LowPart = luid.low;
      l.HighPart = luid.high;
      com_ptr<IDXGIAdapter1> adapter;
      hr = f4->EnumAdapterByLuid(l, __uuidof(IDXGIAdapter1), adapter.put_void());
      if (FAILED(hr)) {
        return hr;
      }
      const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
      return D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION, dev.put(), nullptr, ctx ? ctx->put() : nullptr);
    }

    // ── generation 資源 ────────────────────────────────────────────────────
    /**
     * @brief 一個 generation 的 server 端資源。以 shared_ptr 持有：bridge（本 generation）、tracking 寫入端、
     *        frame_source_t 各持一份，最後一份放掉才 unmap、關 handle、釋放 D3D 物件。
     *        已經移交給 vrserver 的 handle（WELCOME／TEXTURES 裡的值）不在這裡，也從不由 server 關閉（§B.1 所有權）。
     */
    struct gen_t {
      uint64_t generation = 0;
      handle_t section;
      handle_t evt_trk;
      handle_t evt_frm;
      vripc_shm_t *shm = nullptr;  ///< MapViewOfFile（FILE_MAP_READ|FILE_MAP_WRITE）
      bool has_config = false;
      vripc_session_config_t config {};
      // ring 與 fence（bridge 的 device 上建立；server 自己的 NT handle 留給 display_vr_t）
      com_ptr<ID3D11Texture2D> tex[VRIPC_TEX_COUNT];
      com_ptr<ID3D11Fence> shared_fence;
      com_ptr<ID3D11Fence> consumed_fence;
      handle_t tex_nt[VRIPC_TEX_COUNT];
      handle_t shared_fence_nt;
      handle_t consumed_fence_nt;
      bool textures = false;
      std::atomic<bool> alive {true};
      uint64_t trk_write_index = 0;  ///< tracking 寫入端的本地計數（只在 trk_mtx_ 內存取；絕不從 shm 讀回）
      bool trk_have_last = false;  ///< 本 generation 已寫過樣本（trk_mtx_）
      uint32_t trk_last_id = 0;  ///< 最近寫進 ring 的 sampleId（trk_mtx_）

      ~gen_t() {
        if (shm) {
          UnmapViewOfFile(shm);
          shm = nullptr;
        }
      }
    };

    std::shared_ptr<gen_t> gen_of(const frame_source_t &src) {
      return std::static_pointer_cast<gen_t>(src.impl);
    }

    /// 一條已驗證的連線（WELCOME 之後到 TEARDOWN）
    struct conn_t {
      handle_t proc;  ///< vrserver（或 selftest peer）：QUERY_LIMITED|DUP_HANDLE|SYNCHRONIZE，保留到 generation 結束
      DWORD pid = 0;
      FILETIME created {};
      bool selftest_peer = false;
      uint32_t driver_version_packed = 0;
      uint32_t driver_caps = 0;
      std::string iface;
      uint32_t out_seq = 0;
      uint32_t in_seq = 0;
      std::shared_ptr<gen_t> gen;
      bool textures_sent = false;
      bool ready = false;
      int64_t welcome_qpc = 0;
      int64_t ready_deadline = 0;
      bool armed_sent = false;
      uint64_t pacing_seq = 0;  ///< 單格 seqlock 的本地 seq（偶數 = 完成）
      int64_t next_heartbeat = 0;
      // SPSC 消費端（以本地 tail 為準，不信 shm 的 tail）
      uint64_t haptic_tail = 0;
      uint64_t timing_tail = 0;
      uint64_t log_tail = 0;
      uint32_t ring_corrupt = 0;
      // 限速
      int64_t log_window = 0;
      uint32_t log_in_window = 0;
      uint64_t log_suppressed = 0;
      int64_t haptic_window = 0;
      uint32_t haptic_in_window[2] {};
      int64_t state_log_window = 0;
      uint32_t state_logs = 0;
      // timing 10 s 統計
      int64_t timing_window = 0;
      uint64_t timing_counts[16] {};
      uint64_t timing_total = 0;
      vripc_driver_status_t last_driver {};  ///< 最近一次複製的 driver_status（已驗證的欄位另存 status_）
    };

    struct peer_allow_t {
      handle_t proc;
      DWORD pid = 0;
      FILETIME created {};
      std::wstring image;
    };

    struct recent_t {
      uint32_t id = 0;
      int64_t qpc = 0;
    };

    enum class io_e {
      ok,
      timeout,
      too_big,  ///< ERROR_MORE_DATA：訊息超過 VRIPC_PIPE_MAX_MSG
      failed,
      stopped,
    };

    struct verify_result_t {
      bool ok = false;
      uint32_t reject = 0;  ///< 0 = 不送 REJECT（不是本協定）
      uint32_t detail = 0;
      bool abi_mismatch = false;
      uint32_t peer_abi = 0;
      std::string reason;
      conn_t conn;
    };

    struct commands_t {
      bool config_changed = false;
      bool quit = false;
      std::optional<uint32_t> v2p;
      std::optional<std::pair<uint64_t, uint32_t>> teardown;
      std::optional<vripc_pacing_t> pacing;
    };

    void fill_hdr(vripc_msg_hdr_t &h, uint16_t type, uint32_t size, uint32_t seq) {
      h.magic = VRIPC_MAGIC_PIPE;
      h.type = type;
      h.flags = 0;
      h.size = size;
      h.seq = seq;
    }

    // ── bridge 本體 ─────────────────────────────────────────────────────────
    class bridge_t {
    public:
      bool start();
      void stop();
      status_t status();
      bool set_session_config(const vripc_session_config_t &cfg);
      void clear_session_config();
      std::optional<luid_t> session_config_luid();
      void set_armed(bool armed);
      void request_quit();
      void set_pacing(const vripc_pacing_t &p);
      void set_pacing_ppm(int32_t ppm);
      void set_dev_mode(bool on);
      bool send_v2p(uint32_t us);
      void user_changed();
      bool allow_peer(std::uintptr_t child, const FILETIME *expected_created = nullptr);
      void revoke_peer();
      counters_t counters();
      selftest_objects_t duplicate_objects();
      void set_host_image(const std::wstring &path);
      void request_teardown(uint64_t generation, uint32_t reason);
      void set_event_sink(event_cb cb);
      void set_haptic_sink(haptic_cb cb);
      void set_timing_sink(timing_cb cb);
      bool tracking_wanted() const;
      void publish(const tracked_sample_t &s);
      bool sample_recent(uint32_t id, int64_t now, int64_t *published_qpc = nullptr);
      std::shared_ptr<frame_source_t> source();
      pipe_sd_report_t check_pipe_security(bool roundtrip);

    private:
      void thread_main();
      void shutdown_all();
      bool create_pipe();
      void refresh_user();
      DWORD apply_user_dacl(const std::optional<user_t> &u);
      bool pipe_has_user_ace(const sid_t &logon);
      void start_listen();
      void stop_listen();
      void on_read_event();
      void on_connected();
      verify_result_t verify(const FILETIME &connected_at);
      void reject_and_disconnect(verify_result_t &r);
      void handshake(conn_t &&c);
      bool ensure_device(const luid_t &luid, HRESULT &hr);
      bool create_textures(gen_t &g, HRESULT &hr);
      void send_textures();
      void handle_message(const uint8_t *buf, DWORD n);
      void handle_state(const vripc_state_t &s);
      void process_commands();
      void periodic(int64_t now);
      bool snapshot_driver(int64_t now);
      void drain_rings(int64_t now);
      void write_pacing(conn_t &c, const vripc_pacing_t &p);
      void write_default_pacing(conn_t &c);
      void send_state(uint32_t kind, uint32_t arg);
      bool send(const void *msg, DWORD size);
      void protocol_error(const std::string &reason, uint32_t detail);
      void teardown(const std::string &reason, bool send_bye, uint32_t bye_reason, uint32_t bye_detail, bool wait_close = true);
      void close_wait(DWORD ms);
      io_e issue_read(uint8_t *buf, DWORD cap);
      io_e complete_read(DWORD &n);
      void cancel_read();
      io_e read_sync(uint8_t *buf, DWORD cap, DWORD timeout_ms, DWORD &n);
      bool write_msg(const void *msg, DWORD size);
      void start_read();
      void arm_timer();
      void disarm_timer();
      DWORD compute_timeout(int64_t now, int64_t next_pipe_try, int64_t next_user_poll) const;
      void set_phase(phase_e p);
      void emit(event_t e);

      // 生命週期
      std::mutex life_mtx_;
      std::thread thread_;
      handle_t ev_stop_;  ///< manual-reset
      handle_t ev_cmd_;  ///< auto-reset
      handle_t ev_read_;  ///< manual-reset（overlapped 用）
      handle_t ev_write_;  ///< manual-reset（overlapped 用）
      handle_t timer_;  ///< 連線中 2 ms 週期（高解析度 waitable timer）
      bool timer_armed_ = false;

      // pipe（handle 的生命週期與 SD 操作以 pipe_mtx_ 保護；IO 只在 bridge 執行緒）
      std::mutex pipe_mtx_;
      handle_t pipe_;
      OVERLAPPED ov_read_ {};
      OVERLAPPED ov_write_ {};
      bool listening_ = false;
      bool read_pending_ = false;
      DWORD last_io_error_ = 0;
      std::array<uint8_t, VRIPC_PIPE_MAX_MSG> rbuf_ {};

      // bridge 執行緒的狀態
      DWORD self_session_ = 0xFFFFFFFFu;
      std::optional<user_t> user_;
      std::optional<conn_t> conn_;
      bool in_teardown_ = false;
      uint32_t streak_ = 0;
      int64_t backoff_until_ = 0;
      uint64_t generation_counter_ = 0;
      std::string pipe_reason_logged_;
      std::atomic<bool> user_changed_ {false};

      // D3D（bridge 執行緒建立 ring／fence 用）
      com_ptr<ID3D11Device> bdev_;
      com_ptr<ID3D11Device5> bdev5_;
      luid_t bdev_luid_;

      // 期望狀態、命令、快照（mtx_）
      std::mutex mtx_;
      status_t status_;
      counters_t counters_;
      std::shared_ptr<gen_t> cur_gen_;  ///< 目前連線的 generation（duplicate_objects() 用；bridge 執行緒寫）
      commands_t cmds_;
      std::optional<vripc_session_config_t> desired_config_;
      bool desired_armed_ = false;
      bool dev_mode_ = false;
      std::optional<vripc_pacing_t> explicit_pacing_;
      uint32_t last_refresh_mhz_ = 0;
      std::optional<peer_allow_t> peer_allow_;
      std::wstring host_image_;
      sid_t user_logon_copy_;  ///< check_pipe_security() 用（bridge 執行緒寫、呼叫端讀）
      std::shared_ptr<frame_source_t> source_;
      event_cb event_sink_;
      haptic_cb haptic_sink_;
      timing_cb timing_sink_;

      // tracking 寫入端（K26：trk_mtx_ 是唯一的寫入臨界區）
      mutable std::mutex trk_mtx_;
      std::shared_ptr<gen_t> trk_gen_;
      std::array<recent_t, kRecentSamples> recent_ {};
      uint64_t recent_head_ = 0;
      std::atomic<bool> tracking_wanted_ {false};
      std::atomic<uint64_t> trk_published_ {0};
      std::atomic<uint64_t> trk_dropped_ {0};
    };

    bridge_t &bridge() {
      // 刻意不解構（行程結束時由 kernel 收）：避免 static 解構順序與執行緒 join 的問題。
      // 正常關閉路徑是 main 呼叫 stop()。
      static bridge_t *b = new bridge_t();
      return *b;
    }

    // ── 生命週期 ────────────────────────────────────────────────────────────
    bool bridge_t::start() {
      if (config::vr.pcvr == config::vr_t::pcvr_e::disabled) {
        return false;  // 不變式 5：disabled 時完全不動
      }
      std::lock_guard lk(life_mtx_);
      if (thread_.joinable()) {
        return true;
      }
      if (!ev_stop_) {
        ev_stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ev_cmd_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        ev_read_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ev_write_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!t) {
          t = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }
        timer_.reset(t);
      }
      if (!ev_stop_ || !ev_cmd_ || !ev_read_ || !ev_write_) {
        BOOST_LOG(error) << "[VIPLE-VR-IPC] disabled reason=event-create-failed err=" << GetLastError();
        return false;
      }
      ResetEvent(ev_stop_.get());
      {
        std::lock_guard lk2(mtx_);
        status_.running = true;
        status_.phase = phase_e::no_pipe;
      }
      thread_ = std::thread(&bridge_t::thread_main, this);
      return true;
    }

    void bridge_t::stop() {
      std::lock_guard lk(life_mtx_);
      if (!thread_.joinable()) {
        return;
      }
      SetEvent(ev_stop_.get());
      thread_.join();
      std::lock_guard lk2(mtx_);
      status_.running = false;
      status_.phase = phase_e::stopped;
    }

    void bridge_t::set_phase(phase_e p) {
      std::lock_guard lk(mtx_);
      status_.phase = p;
    }

    void bridge_t::emit(event_t e) {
      event_cb cb;
      {
        std::lock_guard lk(mtx_);
        cb = event_sink_;
      }
      if (cb) {
        cb(e);
      }
    }

    status_t bridge_t::status() {
      status_t s;
      {
        std::lock_guard lk(mtx_);
        s = status_;
        s.dev_mode = dev_mode_;
        s.armed = desired_armed_;
      }
      s.trk_published = trk_published_.load(std::memory_order_relaxed);
      s.trk_dropped = trk_dropped_.load(std::memory_order_relaxed);
      return s;
    }

    // ── 控制 API（只改期望狀態、喚醒 bridge 執行緒）───────────────────────────
    bool bridge_t::set_session_config(const vripc_session_config_t &in) {
      vripc_session_config_t cfg = in;
      const bool shape_ok = cfg.eye_width >= 1 && cfg.eye_width <= 8192 && cfg.eye_height >= 1 && cfg.eye_height <= 8192 &&
                            cfg.packed_width == 2 * cfg.eye_width && cfg.packed_height == cfg.eye_height &&
                            cfg.dxgi_format == VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM && cfg.refresh_mhz >= 60000 && cfg.refresh_mhz <= 240000 &&
                            (cfg.adapter_luid_low != 0 || cfg.adapter_luid_high != 0) && cfg.universe_id <= 0xFFFFFFFFull;
      if (!shape_ok) {
        BOOST_LOG(warning) << "[VIPLE-VR-IPC] session config rejected eye=" << cfg.eye_width << 'x' << cfg.eye_height
                           << " packed=" << cfg.packed_width << 'x' << cfg.packed_height << " fmt=" << cfg.dxgi_format
                           << " refreshMhz=" << cfg.refresh_mhz;
        return false;
      }
      {
        std::lock_guard lk(mtx_);
        if (!dev_mode_) {
          cfg.dev_flags = 0;
          cfg.dev_arg = 0;
        }
        cfg.audio_endpoint_id[127] = 0;  // 強制 NUL 結尾
        desired_config_ = cfg;
        explicit_pacing_.reset();
        last_refresh_mhz_ = cfg.refresh_mhz;
        cmds_.config_changed = true;
      }
      SetEvent(ev_cmd_.get());
      return true;
    }

    void bridge_t::clear_session_config() {
      {
        std::lock_guard lk(mtx_);
        desired_config_.reset();
        explicit_pacing_.reset();
        cmds_.config_changed = true;
      }
      SetEvent(ev_cmd_.get());
    }

    std::optional<luid_t> bridge_t::session_config_luid() {
      std::lock_guard lk(mtx_);
      if (!desired_config_) {
        return std::nullopt;
      }
      return luid_t {desired_config_->adapter_luid_low, desired_config_->adapter_luid_high};
    }

    void bridge_t::set_armed(bool armed) {
      {
        std::lock_guard lk(mtx_);
        desired_armed_ = armed;
      }
      SetEvent(ev_cmd_.get());
    }

    void bridge_t::request_quit() {
      {
        std::lock_guard lk(mtx_);
        cmds_.quit = true;
      }
      SetEvent(ev_cmd_.get());
    }

    void bridge_t::set_pacing(const vripc_pacing_t &in) {
      vripc_pacing_t p = in;
      {
        std::lock_guard lk(mtx_);
        if (!dev_mode_) {
          p.mode = VRIPC_PM_PRODUCTION;
        }
        p.slew_ppm_max = std::min<uint32_t>(p.slew_ppm_max, VRIPC_PACING_SLEW_PPM_MAX);
        p.seq = 0;
        explicit_pacing_ = p;
        cmds_.pacing = p;
      }
      SetEvent(ev_cmd_.get());
    }

    void bridge_t::set_pacing_ppm(int32_t ppm) {
      uint32_t mhz;
      {
        std::lock_guard lk(mtx_);
        mhz = last_refresh_mhz_;
      }
      const int32_t c = std::clamp<int32_t>(ppm, -200, 200);
      const uint64_t base = period_q32_for(mhz);
      vripc_pacing_t p {};
      p.period_q32 = (uint64_t) ((double) base * (1.0 + (double) c * 1e-6));
      p.slew_ppm_max = 200;
      p.pacing_flags = 0;  // 沒有 anchor：driver 只跟週期
      p.mode = VRIPC_PM_PRODUCTION;
      p.epoch = 1;  // 與 write_default_pacing 相同，不觸發重新對齊
      set_pacing(p);
    }

    void bridge_t::set_dev_mode(bool on) {
      std::lock_guard lk(mtx_);
      dev_mode_ = on;
    }

    bool bridge_t::send_v2p(uint32_t us) {
      {
        std::lock_guard lk(mtx_);
        if (!dev_mode_ || !status_.connected) {
          return false;
        }
        cmds_.v2p = us;
      }
      SetEvent(ev_cmd_.get());
      return true;
    }

    void bridge_t::user_changed() {
      user_changed_.store(true);
      if (ev_cmd_) {
        SetEvent(ev_cmd_.get());
      }
    }

    void bridge_t::request_teardown(uint64_t generation, uint32_t reason) {
      {
        std::lock_guard lk(mtx_);
        cmds_.teardown = std::make_pair(generation, reason);
      }
      if (ev_cmd_) {
        SetEvent(ev_cmd_.get());
      }
    }

    void bridge_t::set_host_image(const std::wstring &path) {
      std::lock_guard lk(mtx_);
      host_image_ = normalize_path(path);
    }

    void bridge_t::set_event_sink(event_cb cb) {
      std::lock_guard lk(mtx_);
      event_sink_ = std::move(cb);
    }

    void bridge_t::set_haptic_sink(haptic_cb cb) {
      std::lock_guard lk(mtx_);
      haptic_sink_ = std::move(cb);
    }

    void bridge_t::set_timing_sink(timing_cb cb) {
      std::lock_guard lk(mtx_);
      timing_sink_ = std::move(cb);
    }

    std::shared_ptr<frame_source_t> bridge_t::source() {
      std::lock_guard lk(mtx_);
      return source_;
    }

    counters_t bridge_t::counters() {
      std::lock_guard lk(mtx_);
      counters_t c = counters_;
      c.reject_streak = status_.reject_streak;
      return c;
    }

    selftest_objects_t bridge_t::duplicate_objects() {
      selftest_objects_t o;
      // 最小權限複製（縱深防禦）：selftest 只讀 SD（owner／DACL／label 只要 READ_CONTROL）與 pipe 名稱，
      // 拿不到 WRITE_DAC——pipe 的 DACL 只有 bridge 能在 pipe_mtx_ 內改（refresh_user、check_pipe_security）。
      // 要求的權限是來源 handle 的子集，DuplicateHandle 不再做存取檢查。
      auto dup = [](HANDLE h, DWORD access) -> void * {
        HANDLE out = nullptr;
        if (h && h != INVALID_HANDLE_VALUE && DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &out, access, FALSE, 0)) {
          return out;
        }
        return nullptr;
      };
      {
        std::lock_guard lk(pipe_mtx_);
        // FILE_READ_ATTRIBUTES：保守起見一併給，讓 GetFileInformationByHandleEx(FileNameInfo) 在 NPFS 上一定查得到名稱
        o.pipe = dup(pipe_.get(), READ_CONTROL | FILE_READ_ATTRIBUTES);
      }
      std::shared_ptr<gen_t> g;
      {
        std::lock_guard lk(mtx_);
        g = cur_gen_;
      }
      if (g) {
        o.shm = dup(g->section.get(), READ_CONTROL);
        o.evt_trk = dup(g->evt_trk.get(), READ_CONTROL);
        o.evt_frm = dup(g->evt_frm.get(), READ_CONTROL);
      }
      return o;
    }

    bool bridge_t::allow_peer(std::uintptr_t child, const FILETIME *expected_created) {
      HANDLE src = reinterpret_cast<HANDLE>(child);
      if (!src || src == INVALID_HANDLE_VALUE) {
        return false;
      }
      HANDLE dup = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), src, GetCurrentProcess(), &dup, PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, 0)) {
        BOOST_LOG(warning) << "[VIPLE-VR-IPC] selftest-peer refused reason=dup-handle err=" << GetLastError();
        return false;
      }
      peer_allow_t p;
      p.proc.reset(dup);
      p.pid = GetProcessId(dup);
      FILETIME ex, kt, ut;
      if (p.pid == 0 || !GetProcessTimes(dup, &p.created, &ex, &kt, &ut)) {
        BOOST_LOG(warning) << "[VIPLE-VR-IPC] selftest-peer refused reason=process-info err=" << GetLastError();
        return false;
      }
      if (expected_created && (expected_created->dwLowDateTime || expected_created->dwHighDateTime) &&
          CompareFileTime(expected_created, &p.created) != 0) {
        BOOST_LOG(warning) << "[VIPLE-VR-IPC] selftest-peer refused reason=creation-time pid=" << p.pid;
        return false;
      }
      p.image = process_image(dup);
      std::wstring expect = install_dir();
      if (!expect.empty()) {
        expect += L"\\tools\\vr_probe\\vr_probe.exe";
      }
      if (!path_equal(p.image, expect)) {
        BOOST_LOG(warning) << "[VIPLE-VR-IPC] selftest-peer refused reason=image image=" << loggable_path(p.image) << " expected=" << loggable_path(expect);
        return false;
      }
      BOOST_LOG(info) << "[VIPLE-VR-IPC] selftest-peer allowed pid=" << p.pid;
      std::lock_guard lk(mtx_);
      peer_allow_ = std::move(p);
      return true;
    }

    void bridge_t::revoke_peer() {
      std::lock_guard lk(mtx_);
      if (peer_allow_) {
        BOOST_LOG(info) << "[VIPLE-VR-IPC] selftest-peer revoked pid=" << peer_allow_->pid;
      }
      peer_allow_.reset();
    }

    // ── tracking（K26）──────────────────────────────────────────────────────
    bool bridge_t::tracking_wanted() const {
      return tracking_wanted_.load(std::memory_order_acquire);
    }

    void bridge_t::publish(const tracked_sample_t &s) {
      if (!tracking_wanted_.load(std::memory_order_acquire)) {
        return;
      }
      for (const auto &p : s.pose) {
        if (!finite_pose(p)) {
          trk_dropped_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }

      vripc_tracking_slot_t tmp {};
      tmp.sample_id = s.sample_id;
      tmp.space_epoch = s.space_epoch;
      tmp.flags = s.flags;
      tmp.sample_time_ns = s.sample_time_ns;
      tmp.arrival_qpc = s.arrival_qpc;
      tmp.target_server_qpc = s.target_server_qpc;
      tmp.predict_ns = s.predict_ns;
      std::memcpy(tmp.pose, s.pose, sizeof tmp.pose);
      std::memcpy(tmp.input, s.input, sizeof tmp.input);
      tmp.gaze_yaw_f16 = s.gaze_yaw_f16;
      tmp.gaze_pitch_f16 = s.gaze_pitch_f16;
      tmp.gaze_conf = s.gaze_conf;
      tmp.gaze_flags = s.gaze_flags;

      std::lock_guard lk(trk_mtx_);
      const auto g = trk_gen_;
      if (!g || !g->shm) {
        return;
      }
      // 0x5506 handler 在 ENet 與 picoquic 兩條執行緒上跑：on_tracking() 接受的順序與這裡取得 writer mutex 的順序
      // 可能相反。ring 的最新一筆必須是最新的樣本，所以在單一寫入臨界區內丟掉「不比上一筆新」的樣本
      // （迴繞安全；倒退超過 1024 視為 client 重置 sampleId，與 vr_session 的 kSampleIdResetThreshold 相同）。
      if (g->trk_have_last) {
        const int32_t delta = (int32_t) (s.sample_id - g->trk_last_id);
        if (delta <= 0 && delta > -1024) {
          trk_dropped_.fetch_add(1, std::memory_order_relaxed);
          return;
        }
      }
      g->trk_have_last = true;
      g->trk_last_id = s.sample_id;
      auto &ring = g->shm->tracking;
      const uint64_t idx = g->trk_write_index;
      auto *slot = &ring.slot[idx % VRIPC_TRK_SLOTS];
      // §B.3 多格 seqlock 寫入端：奇數 seq（之後有屏障）→ payload → 偶數 seq → write_index
      vripc_seq_write_begin(&slot->seq, 2 * idx + 1);
      std::memcpy(reinterpret_cast<char *>(slot) + sizeof(uint64_t), reinterpret_cast<const char *>(&tmp) + sizeof(uint64_t), sizeof(tmp) - sizeof(uint64_t));
      vripc_store_release_u64(&slot->seq, 2 * idx + 2);
      vripc_store_release_u64(&ring.write_index, idx + 1);
      g->trk_write_index = idx + 1;
      SetEvent(g->evt_trk.get());

      recent_[recent_head_ % kRecentSamples] = recent_t {s.sample_id, now_qpc()};
      ++recent_head_;
      trk_published_.fetch_add(1, std::memory_order_relaxed);
    }

    bool bridge_t::sample_recent(uint32_t id, int64_t now, int64_t *published_qpc) {
      if (id == 0) {
        return false;
      }
      const int64_t window = 2 * qpf();
      std::lock_guard lk(trk_mtx_);
      const uint64_t count = std::min<uint64_t>(recent_head_, kRecentSamples);
      for (uint64_t i = 0; i < count; ++i) {
        const auto &e = recent_[(recent_head_ - 1 - i) % kRecentSamples];
        if (now - e.qpc > window) {
          break;
        }
        if (e.id == id) {
          if (published_qpc) {
            *published_qpc = e.qpc;
          }
          return true;
        }
      }
      return false;
    }

    // ── pipe IO（只在 bridge 執行緒）────────────────────────────────────────
    io_e bridge_t::issue_read(uint8_t *buf, DWORD cap) {
      ov_read_ = {};
      ov_read_.hEvent = ev_read_.get();
      ResetEvent(ev_read_.get());
      // overlapped handle：同步完成（TRUE）時 event 同樣會被設起來，一律等 event 再取結果
      if (ReadFile(pipe_.get(), buf, cap, nullptr, &ov_read_)) {
        read_pending_ = true;
        return io_e::ok;
      }
      const DWORD e = GetLastError();
      if (e == ERROR_IO_PENDING) {
        read_pending_ = true;
        return io_e::ok;
      }
      last_io_error_ = e;
      return e == ERROR_MORE_DATA ? io_e::too_big : io_e::failed;
    }

    io_e bridge_t::complete_read(DWORD &n) {
      read_pending_ = false;
      n = 0;
      if (GetOverlappedResult(pipe_.get(), &ov_read_, &n, FALSE)) {
        return io_e::ok;
      }
      const DWORD e = GetLastError();
      last_io_error_ = e;
      return e == ERROR_MORE_DATA ? io_e::too_big : io_e::failed;
    }

    void bridge_t::cancel_read() {
      if (!read_pending_) {
        return;
      }
      CancelIoEx(pipe_.get(), &ov_read_);
      DWORD n = 0;
      GetOverlappedResult(pipe_.get(), &ov_read_, &n, TRUE);
      read_pending_ = false;
    }

    io_e bridge_t::read_sync(uint8_t *buf, DWORD cap, DWORD timeout_ms, DWORD &n) {
      n = 0;
      if (auto r = issue_read(buf, cap); r != io_e::ok) {
        return r;
      }
      HANDLE hs[2] = {ev_read_.get(), ev_stop_.get()};
      const DWORD w = WaitForMultipleObjects(2, hs, FALSE, timeout_ms);
      if (w == WAIT_OBJECT_0) {
        return complete_read(n);
      }
      cancel_read();
      return w == WAIT_OBJECT_0 + 1 ? io_e::stopped : io_e::timeout;
    }

    bool bridge_t::write_msg(const void *msg, DWORD size) {
      ov_write_ = {};
      ov_write_.hEvent = ev_write_.get();
      ResetEvent(ev_write_.get());
      if (!WriteFile(pipe_.get(), msg, size, nullptr, &ov_write_)) {
        const DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
          last_io_error_ = e;
          return false;
        }
      }
      // 不可信的一方不讀 pipe 時寫入會一直 pending：100 ms 就取消（永遠不 FlushFileBuffers，sec-M8）
      if (WaitForSingleObject(ev_write_.get(), kWriteTimeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(pipe_.get(), &ov_write_);
        DWORD x = 0;
        GetOverlappedResult(pipe_.get(), &ov_write_, &x, TRUE);
        last_io_error_ = WAIT_TIMEOUT;
        return false;
      }
      DWORD done = 0;
      if (!GetOverlappedResult(pipe_.get(), &ov_write_, &done, FALSE)) {
        last_io_error_ = GetLastError();
        return false;
      }
      return done == size;
    }

    bool bridge_t::send(const void *msg, DWORD size) {
      if (!conn_) {
        return false;
      }
      if (write_msg(msg, size)) {
        return true;
      }
      if (!in_teardown_) {
        // 對方不讀 pipe（寫入逾時）時等它關閉沒有意義：直接拆，整體 ≤ 100 ms（寫入）+ 取消
        teardown(std::format("write-failed err={}", last_io_error_), false, 0, 0, false);
      }
      return false;
    }

    void bridge_t::start_read() {
      if (!conn_ || read_pending_) {
        return;
      }
      const auto r = issue_read(rbuf_.data(), (DWORD) rbuf_.size());
      if (r == io_e::too_big) {
        protocol_error("message-too-big", VRIPC_REJ_BAD_MESSAGE);
      } else if (r != io_e::ok) {
        teardown(std::format("pipe-closed err={}", last_io_error_), false, 0, 0);
      }
    }

    /// REJECT／BYE 之後：等對方關閉最多 ms（收到的資料一律丟掉），然後取消 pending read
    void bridge_t::close_wait(DWORD ms) {
      const int64_t deadline = now_qpc() + ms_to_qpc(ms);
      for (;;) {
        if (!read_pending_ && issue_read(rbuf_.data(), (DWORD) rbuf_.size()) != io_e::ok) {
          break;
        }
        const int64_t rem = deadline - now_qpc();
        if (rem <= 0) {
          break;
        }
        if (WaitForSingleObject(ev_read_.get(), ceil_ms(rem)) != WAIT_OBJECT_0) {
          break;
        }
        DWORD n = 0;
        if (complete_read(n) != io_e::ok) {
          break;  // ERROR_BROKEN_PIPE 等：對方已關
        }
      }
      cancel_read();
    }

    void bridge_t::arm_timer() {
      if (!timer_ || timer_armed_) {
        return;
      }
      LARGE_INTEGER due;
      due.QuadPart = -20000;  // 2 ms（100 ns 單位，相對）
      timer_armed_ = SetWaitableTimer(timer_.get(), &due, 2, nullptr, nullptr, FALSE) != FALSE;
    }

    void bridge_t::disarm_timer() {
      if (timer_ && timer_armed_) {
        CancelWaitableTimer(timer_.get());
      }
      timer_armed_ = false;
    }

    // ── pipe 建立與使用者 ACE（§B.1、K3）──────────────────────────────────────
    bool bridge_t::create_pipe() {
      DWORD self = 0xFFFFFFFFu;
      if (!ProcessIdToSessionId(GetCurrentProcessId(), &self)) {
        self = 0xFFFFFFFFu;
      }
      const DWORD console = WTSGetActiveConsoleSessionId();
      self_session_ = self;
      {
        std::lock_guard lk(mtx_);
        status_.self_session = self;
        status_.console_session = console;
      }

      std::string reason;
      if (self == 0xFFFFFFFFu || self != console) {
        reason = "session-mismatch";
      } else {
        // 建立時 DACL 只有 SY（沒有人能連），使用者登入後才以 SetSecurityInfo 加 logon SID ACE。
        // O:SY 讓 driver 能以「擁有者 = S-1-5-18」驗證 server（medium 行程建不出擁有者為 SYSTEM 的 pipe，實測 1307）。
        local_sd_t sd;
        if (!sd.from_sddl(L"O:SYG:SYD:P(A;;GA;;;SY)S:(ML;;NWNR;;;ME)")) {
          reason = std::format("error-{}", GetLastError());
        } else {
          SECURITY_ATTRIBUTES sa {sizeof(SECURITY_ATTRIBUTES), sd.p, FALSE};
          const std::wstring name = std::wstring(VRIPC_PIPE_NAME_PREFIX) + std::to_wstring(self);
          HANDLE h = CreateNamedPipeW(
            name.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED | WRITE_DAC,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            kPipeBufferBytes,
            kPipeBufferBytes,
            0,
            &sa
          );
          if (h == INVALID_HANDLE_VALUE) {
            const DWORD e = GetLastError();
            if (e == ERROR_INVALID_OWNER) {
              reason = "not-system";  // 例：console 模式（預期的 fail closed）
            } else if (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY) {
              reason = "squatted";  // FIRST_PIPE_INSTANCE 失敗：名稱已被別人建立
            } else {
              reason = std::format("error-{}", e);
            }
          } else {
            std::lock_guard lk(pipe_mtx_);
            pipe_.reset(h);
          }
        }
      }

      if (!reason.empty()) {
        {
          std::lock_guard lk(mtx_);
          status_.pipe_reason = reason;
          status_.phase = phase_e::no_pipe;
        }
        if (reason != pipe_reason_logged_) {
          pipe_reason_logged_ = reason;
          BOOST_LOG(warning) << "[VIPLE-VR-IPC] disabled reason=" << reason << " self=" << (int64_t) (int32_t) self
                             << " console=" << (int64_t) (int32_t) console << " (retry every 30s)";
          emit({event_e::pipe_state, 0, 0, 0, reason});
        }
        return false;
      }

      {
        std::lock_guard lk(mtx_);
        status_.pipe_reason = "ok";
        status_.phase = phase_e::no_user;
      }
      pipe_reason_logged_ = "ok";
      BOOST_LOG(info) << "[VIPLE-VR-IPC] pipe ready session=" << self << " dacl=system-only";
      emit({event_e::pipe_state, 0, 0, 0, "ok"});
      return true;
    }

    void bridge_t::refresh_user() {
      const DWORD console = WTSGetActiveConsoleSessionId();
      std::optional<user_t> u;
      if (console == self_session_) {
        u = query_session_user(self_session_);
      }
      {
        std::lock_guard lk(mtx_);
        status_.console_session = console;
      }
      const bool same = (u && user_ && sid_equal(u->logon, user_->logon)) || (!u && !user_);
      if (same) {
        // 使用者沒變：確認 pipe 實際的 DACL 仍帶這個使用者的 ACE。有人在 pipe_mtx_ 之外把舊 DACL 寫回去（例：拿
        // WRITE_DAC 的複本做 SetSecurityInfo 來回，剛好夾在換 ACE 前後）時，否則要等到下次換人才會恢復——期間
        // user_ 已是新使用者、照常 listen，vrserver 卻一直 ACCESS_DENIED，而且 log 裡沒有任何錯誤行。每次輪詢自我修復。
        if (user_ && !pipe_has_user_ace(user_->logon)) {
          const DWORD err = apply_user_dacl(user_);
          if (err == ERROR_SUCCESS) {
            BOOST_LOG(warning) << "[VIPLE-VR-IPC] user-ace reapplied sid=*" << sid_rid(as_psid(user_->user)) << " (pipe DACL was missing the console user's ACE)";
          } else {
            BOOST_LOG(error) << "[VIPLE-VR-IPC] user-ace reapply failed err=" << err;
          }
        }
        return;
      }

      // 使用者變了（登入、登出、換人）：先斷掉現有連線與 listen，再換 ACE
      if (conn_) {
        teardown("user-changed", true, VRIPC_BYE_NORMAL, 0);
      }
      stop_listen();

      const DWORD err = apply_user_dacl(u);
      const bool ok = err == ERROR_SUCCESS;
      if (!ok) {
        // 換不了 ACE 就當成沒有使用者：不 listen，沒有人連得進來（fail closed）
        BOOST_LOG(error) << "[VIPLE-VR-IPC] user-ace update failed err=" << err;
        user_.reset();
        std::lock_guard lk(mtx_);
        status_.user_present = false;
        status_.user_rid = 0;
        user_logon_copy_.clear();
        return;
      }

      const uint32_t rid = u ? sid_rid(as_psid(u->user)) : 0;
      if (u) {
        BOOST_LOG(info) << "[VIPLE-VR-IPC] user-ace set sid=*" << rid;
      } else {
        BOOST_LOG(info) << "[VIPLE-VR-IPC] user-ace cleared (no console user)";
      }
      user_ = std::move(u);
      std::lock_guard lk(mtx_);
      status_.user_present = user_.has_value();
      status_.user_rid = rid;
      user_logon_copy_ = user_ ? user_->logon : sid_t {};
    }

    DWORD bridge_t::apply_user_dacl(const std::optional<user_t> &u) {
      // protected DACL：SY 完整權限＋（有使用者時）主控台使用者 logon SID 的 0x12019b（K3）
      const auto sys = well_known_sid(WinLocalSystemSid);
      const DWORD acl_size = (DWORD) ((sizeof(ACL) + 2 * sizeof(ACCESS_ALLOWED_ACE) + sys.size() + (u ? u->logon.size() : 0) + 16 + 3) & ~size_t {3});
      std::vector<uint8_t> acl_buf(acl_size, 0);
      auto *acl = reinterpret_cast<PACL>(acl_buf.data());
      const bool built = !sys.empty() && InitializeAcl(acl, acl_size, ACL_REVISION) &&
                         AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, as_psid(sys)) &&
                         (!u || AddAccessAllowedAce(acl, ACL_REVISION, kUserPipeAccess, as_psid(u->logon)));
      if (!built) {
        const DWORD e = GetLastError();
        return e != ERROR_SUCCESS ? e : ERROR_INVALID_ACL;
      }
      std::lock_guard lk(pipe_mtx_);
      return SetSecurityInfo(pipe_.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    }

    bool bridge_t::pipe_has_user_ace(const sid_t &logon) {
      std::lock_guard lk(pipe_mtx_);
      if (!pipe_) {
        return true;  // 沒有 pipe：沒有東西要修
      }
      PACL dacl = nullptr;
      PSECURITY_DESCRIPTOR sd = nullptr;
      if (GetSecurityInfo(pipe_.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return true;  // 讀不到就不動（下次輪詢再試），避免在查詢失敗時反覆改寫
      }
      bool found = false;
      for (DWORD i = 0; dacl && i < dacl->AceCount && !found; ++i) {
        void *p = nullptr;
        if (!GetAce(dacl, i, &p) || static_cast<const ACE_HEADER *>(p)->AceType != ACCESS_ALLOWED_ACE_TYPE) {
          continue;
        }
        auto *a = static_cast<ACCESS_ALLOWED_ACE *>(p);
        found = a->Mask == kUserPipeAccess && sid_equal(reinterpret_cast<PSID>(&a->SidStart), logon);
      }
      LocalFree(sd);
      return found;
    }

    void bridge_t::start_listen() {
      ov_read_ = {};
      ov_read_.hEvent = ev_read_.get();
      ResetEvent(ev_read_.get());
      if (ConnectNamedPipe(pipe_.get(), &ov_read_)) {
        on_connected();  // overlapped 模式一般不會走到這裡
        return;
      }
      const DWORD e = GetLastError();
      if (e == ERROR_IO_PENDING) {
        listening_ = true;
        set_phase(phase_e::listening);
        return;
      }
      if (e == ERROR_PIPE_CONNECTED) {
        on_connected();
        return;
      }
      // ERROR_NO_DATA：client 連上又關了；其他錯誤：斷開後稍後重試
      DisconnectNamedPipe(pipe_.get());
      backoff_until_ = now_qpc() + ms_to_qpc(e == ERROR_NO_DATA ? 100 : 1000);
      if (e != ERROR_NO_DATA) {
        BOOST_LOG(warning) << "[VIPLE-VR-IPC] ConnectNamedPipe failed err=" << e;
      }
    }

    void bridge_t::stop_listen() {
      if (!listening_) {
        return;
      }
      CancelIoEx(pipe_.get(), &ov_read_);
      DWORD n = 0;
      GetOverlappedResult(pipe_.get(), &ov_read_, &n, TRUE);
      listening_ = false;
      DisconnectNamedPipe(pipe_.get());
    }

    // ── 連線、驗證、握手（§B.1、§B.4）────────────────────────────────────────
    void bridge_t::on_read_event() {
      if (listening_) {
        listening_ = false;
        DWORD n = 0;
        if (GetOverlappedResult(pipe_.get(), &ov_read_, &n, FALSE) || GetLastError() == ERROR_PIPE_CONNECTED) {
          on_connected();
        } else {
          DisconnectNamedPipe(pipe_.get());
          backoff_until_ = now_qpc() + ms_to_qpc(100);
        }
        return;
      }
      if (!conn_ || !read_pending_) {
        return;
      }
      DWORD n = 0;
      const auto r = complete_read(n);
      if (r == io_e::ok) {
        handle_message(rbuf_.data(), n);
        start_read();
      } else if (r == io_e::too_big) {
        protocol_error("message-too-big", VRIPC_REJ_BAD_MESSAGE);
      } else {
        // 對方行程結束時 kernel 關掉 client 端 → ERROR_BROKEN_PIPE（kill-server／kill-driver 注入都走這裡）
        teardown(std::format("pipe-closed err={}", last_io_error_), false, 0, 0);
      }
    }

    void bridge_t::on_connected() {
      FILETIME connected_at;
      GetSystemTimeAsFileTime(&connected_at);
      set_phase(phase_e::verifying);
      auto r = verify(connected_at);
      if (!r.ok) {
        reject_and_disconnect(r);
        return;
      }
      streak_ = 0;
      {
        std::lock_guard lk(mtx_);
        status_.reject_streak = 0;
      }
      handshake(std::move(r.conn));
    }

    verify_result_t bridge_t::verify(const FILETIME &connected_at) {
      verify_result_t r;
      auto &c = r.conn;

      // 1. client 行程：開 handle 並保留（PID 重用檢查：建立時間早於連線、仍存活、PID 前後一致）
      ULONG pid = 0;
      if (!GetNamedPipeClientProcessId(pipe_.get(), &pid) || pid == 0) {
        r.reason = std::format("client-pid err={}", GetLastError());
        return r;
      }
      c.pid = pid;
      HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_DUP_HANDLE | SYNCHRONIZE, FALSE, pid);
      if (!hp) {
        r.reason = std::format("open-process err={}", GetLastError());
        return r;
      }
      c.proc.reset(hp);
      FILETIME ex, kt, ut;
      if (!GetProcessTimes(hp, &c.created, &ex, &kt, &ut) || CompareFileTime(&c.created, &connected_at) >= 0) {
        r.reason = "pid-reuse";
        return r;
      }
      if (WaitForSingleObject(hp, 0) != WAIT_TIMEOUT) {
        r.reason = "client-exited";
        return r;
      }
      ULONG pid2 = 0;
      if (!GetNamedPipeClientProcessId(pipe_.get(), &pid2) || pid2 != pid) {
        r.reason = "pid-changed";
        return r;
      }

      // 2. 同一個 session
      ULONG csess = 0xFFFFFFFFu;
      if (!GetNamedPipeClientSessionId(pipe_.get(), &csess) || csess != self_session_) {
        r.reason = std::format("session client={} self={}", (int64_t) (int32_t) csess, self_session_);
        return r;
      }

      // 3. 第一則訊息：以 VRIPC_PIPE_MAX_MSG 為緩衝，先驗凍結的 24 B；不是本協定就直接斷線（不回 REJECT）
      std::array<uint8_t, VRIPC_PIPE_MAX_MSG> buf {};
      DWORD n = 0;
      const auto rr = read_sync(buf.data(), (DWORD) buf.size(), kHelloTimeoutMs, n);
      if (rr == io_e::timeout) {
        r.reject = VRIPC_REJ_TIMEOUT;
        r.reason = "hello-timeout";
        return r;
      }
      if (rr != io_e::ok) {
        r.reason = rr == io_e::too_big ? "not-protocol size>max" : (rr == io_e::stopped ? "stopping" : std::format("hello-read err={}", last_io_error_));
        return r;
      }
      vripc_msg_hdr_t hdr {};
      if (n >= sizeof hdr) {
        std::memcpy(&hdr, buf.data(), sizeof hdr);
      }
      if (n < 24 || hdr.size != n || hdr.magic != VRIPC_MAGIC_PIPE || hdr.type != VRIPC_MSG_HELLO) {
        r.reason = std::format("not-protocol bytes={}", n);
        return r;
      }
      c.in_seq = hdr.seq;
      uint32_t abi = 0;
      std::memcpy(&abi, buf.data() + offsetof(vripc_hello_t, abi_version), sizeof abi);
      r.peer_abi = abi;

      // 4. 身分：連線 token（模擬取得，Identification 等級即可查詢）與行程 token 都要符合主控台使用者
      if (!user_) {
        r.reject = VRIPC_REJ_IDENTITY;
        r.reason = "no-console-user";
        return r;
      }
      handle_t conn_tok;
      {
        pipe_impersonation_t imp(pipe_.get());
        if (!imp.ok()) {
          r.reject = VRIPC_REJ_IDENTITY;
          r.reason = std::format("impersonate err={}", imp.error());
          return r;
        }
        HANDLE t = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t)) {
          conn_tok.reset(t);
        }
      }  // 這裡 RevertToSelf（失敗就 terminate）
      handle_t proc_tok;
      {
        HANDLE t = nullptr;
        if (OpenProcessToken(hp, TOKEN_QUERY, &t)) {
          proc_tok.reset(t);
        }
      }
      if (!conn_tok || !proc_tok) {
        r.reject = VRIPC_REJ_IDENTITY;
        r.reason = std::format("token-open conn={} proc={}", (bool) conn_tok, (bool) proc_tok);
        return r;
      }
      for (const auto *which : {"conn", "proc"}) {
        const auto id = query_token(std::string_view {which} == "conn" ? conn_tok.get() : proc_tok.get());
        const char *fail = nullptr;
        if (!id.ok) {
          fail = "query";
        } else if (!sid_equal(id.user, user_->user)) {
          fail = "user-sid";
        } else if (!sid_equal(id.logon, user_->logon)) {
          fail = "logon-sid";
        } else if (id.integrity < 0x2000) {
          fail = "integrity";
        } else if (id.app_container) {
          fail = "appcontainer";
        }
        if (fail) {
          r.reject = VRIPC_REJ_IDENTITY;
          r.reason = std::format("token-{} {} sid=*{} il={}", which, fail, id.user.empty() ? 0 : sid_rid(as_psid(id.user)), hex32(id.integrity));
          return r;
        }
      }

      // 5. 深度防禦：映像路徑字串比對（不開檔）；selftest peer 例外需 PID＋建立時間＋映像三者相符
      const std::wstring image = process_image(hp);
      std::wstring host;
      std::wstring peer_image;
      bool is_peer = false;
      {
        std::lock_guard lk(mtx_);
        host = host_image_;
        if (peer_allow_ && peer_allow_->pid == pid && CompareFileTime(&peer_allow_->created, &c.created) == 0 && path_equal(image, peer_allow_->image)) {
          is_peer = true;
        }
        if (peer_allow_) {
          peer_image = peer_allow_->image;
        }
      }
      if (!is_peer && !path_equal(image, host)) {
        r.reject = VRIPC_REJ_IDENTITY;
        // r.reason 同時進 sunshine.log 與 event_e::rejected 的 text：對方可控制的映像路徑一律先過濾（loggable_path）
        r.reason = std::format("image image={} expected={}{}", loggable_path(image), loggable_path(host), peer_image.empty() ? std::string {} : " peer=" + loggable_path(peer_image));
        return r;
      }
      c.selftest_peer = is_peer;

      // 6. ABI：先比版本，相等之後才要求大小（sec-M3：任何大小的 HELLO 都能得到 REJECT(ABI_MISMATCH)）
      if (abi != VRIPC_ABI_VERSION) {
        r.reject = VRIPC_REJ_ABI_MISMATCH;
        r.detail = VRIPC_ABI_VERSION;
        r.abi_mismatch = true;
        r.reason = std::format("abi peer={} server={}", abi, VRIPC_ABI_VERSION);
        return r;
      }
      if (n != sizeof(vripc_hello_t)) {
        r.reject = VRIPC_REJ_BAD_MESSAGE;
        r.reason = std::format("hello-size bytes={}", n);
        return r;
      }
      vripc_hello_t hello {};
      std::memcpy(&hello, buf.data(), sizeof hello);
      if (hello.shm_struct_size != sizeof(vripc_shm_t)) {
        r.reject = VRIPC_REJ_ABI_MISMATCH;
        r.detail = (uint32_t) sizeof(vripc_shm_t);
        r.abi_mismatch = true;
        r.reason = std::format("shm-struct peer={} server={}", hello.shm_struct_size, sizeof(vripc_shm_t));
        return r;
      }
      c.driver_version_packed = hello.driver_version_packed;
      c.driver_caps = hello.driver_caps;
      c.iface = sanitize(hello.iface_directmode, VRIPC_IFACE_CHARS);
      r.ok = true;
      return r;
    }

    void bridge_t::reject_and_disconnect(verify_result_t &r) {
      if (r.reject) {
        vripc_reject_t m {};
        fill_hdr(m.hdr, VRIPC_MSG_REJECT, sizeof m, 1);
        m.reason = r.reject;
        m.detail = r.detail;
        write_msg(&m, sizeof m);  // 失敗也無妨：接下來一律斷線
        close_wait(kCloseWaitMs);
      }
      DisconnectNamedPipe(pipe_.get());

      streak_ = std::min<uint32_t>(streak_ + 1, 1000);
      const int64_t backoff_s = reject_backoff_sec(streak_);
      backoff_until_ = now_qpc() + ms_to_qpc(backoff_s * 1000);
      {
        std::lock_guard lk(mtx_);
        counters_.rejects[std::min<uint32_t>(r.reject, std::size(counters_.rejects) - 1)]++;
        status_.rejects++;
        status_.reject_streak = streak_;
        status_.last_reject_reason = r.reject;
        status_.peer_abi_version = r.peer_abi;
        if (r.abi_mismatch) {
          status_.abi_mismatch_seen = true;
        }
        status_.phase = phase_e::backoff;
      }
      BOOST_LOG(warning) << "[VIPLE-VR-IPC] rejected reason=" << r.reason << " rej=" << r.reject << " pid=" << r.conn.pid
                         << " streak=" << streak_ << " backoff=" << backoff_s << 's';
      emit({event_e::rejected, 0, r.reject, 0, r.reason});
      if (r.abi_mismatch) {
        emit({event_e::abi_mismatch, 0, r.peer_abi, 0, r.reason});
      }
    }

    bool bridge_t::ensure_device(const luid_t &luid, HRESULT &hr) {
      hr = S_OK;
      if (bdev_ && bdev_luid_ == luid && bdev_->GetDeviceRemovedReason() == S_OK) {
        return true;
      }
      bdev5_.reset();
      bdev_.reset();
      hr = create_device_on_luid(luid, bdev_, nullptr);
      if (FAILED(hr)) {
        bdev_.reset();
        return false;
      }
      hr = bdev_->QueryInterface(__uuidof(ID3D11Device5), bdev5_.put_void());
      if (FAILED(hr)) {
        bdev_.reset();
        return false;
      }
      bdev_luid_ = luid;
      return true;
    }

    bool bridge_t::create_textures(gen_t &g, HRESULT &hr) {
      D3D11_TEXTURE2D_DESC d {};
      d.Width = g.config.packed_width;
      d.Height = g.config.packed_height;
      d.MipLevels = 1;
      d.ArraySize = 1;
      d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_DEFAULT;
      d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      d.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
      for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
        hr = bdev_->CreateTexture2D(&d, nullptr, g.tex[i].put());
        if (FAILED(hr)) {
          return false;
        }
        com_ptr<IDXGIResource1> res;
        hr = g.tex[i]->QueryInterface(__uuidof(IDXGIResource1), res.put_void());
        if (FAILED(hr)) {
          return false;
        }
        HANDLE h = nullptr;
        hr = res->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
        if (FAILED(hr)) {
          return false;
        }
        g.tex_nt[i].reset(h);
      }
      for (auto *pair : {&g.shared_fence, &g.consumed_fence}) {
        hr = bdev5_->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), pair->put_void());
        if (FAILED(hr)) {
          return false;
        }
        HANDLE h = nullptr;
        // fence 的 shared handle 只接受 GENERIC_ALL（無法降權，靠數值驗證）
        hr = (*pair)->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
        if (FAILED(hr)) {
          return false;
        }
        (pair == &g.shared_fence ? g.shared_fence_nt : g.consumed_fence_nt).reset(h);
      }
      g.textures = true;
      return true;
    }

    void bridge_t::write_pacing(conn_t &c, const vripc_pacing_t &p) {
      if (!c.gen || !c.gen->shm) {
        return;
      }
      auto *dst = &c.gen->shm->pacing;
      // §B.3 單格 seqlock：奇數（之後有屏障）→ payload → 偶數
      c.pacing_seq += 1;
      vripc_seq_write_begin(&dst->seq, c.pacing_seq);
      std::memcpy(reinterpret_cast<char *>(dst) + sizeof(uint64_t), reinterpret_cast<const char *>(&p) + sizeof(uint64_t), sizeof(p) - sizeof(uint64_t));
      c.pacing_seq += 1;
      vripc_store_release_u64(&dst->seq, c.pacing_seq);
    }

    void bridge_t::write_default_pacing(conn_t &c) {
      // idle generation 也必須寫有效週期（drv-B-2）：本 generation config 的 Hz → 上一個 session 的 Hz → 90 Hz
      uint32_t mhz = 0;
      if (c.gen && c.gen->has_config) {
        mhz = c.gen->config.refresh_mhz;
      }
      if (mhz == 0) {
        std::lock_guard lk(mtx_);
        mhz = last_refresh_mhz_;
      }
      vripc_pacing_t p {};
      p.period_q32 = period_q32_for(mhz);
      p.slew_ppm_max = 200;
      p.pacing_flags = 0;  // 沒有 anchor：driver 只跟週期、不追相位
      p.mode = VRIPC_PM_PRODUCTION;
      p.epoch = 1;
      write_pacing(c, p);
    }

    void bridge_t::handshake(conn_t &&cin) {
      conn_t c = std::move(cin);
      std::optional<vripc_session_config_t> cfg;
      bool armed = false;
      bool dev = false;
      std::optional<vripc_pacing_t> pacing;
      {
        std::lock_guard lk(mtx_);
        cfg = desired_config_;
        armed = desired_armed_;
        dev = dev_mode_;
        pacing = explicit_pacing_;
        cmds_.config_changed = false;  // 這個 generation 直接採用目前的期望 config
      }

      auto gen = std::make_shared<gen_t>();
      gen->generation = generation_counter_ + 1;
      gen->has_config = cfg.has_value();
      if (cfg) {
        gen->config = *cfg;
      }

      // S2：不具名 section 與兩個 auto-reset event，SD 只有 SY（方案 B；衛生措施，不是信任邊界）
      std::string fail;
      local_sd_t sd;
      SECURITY_ATTRIBUTES sa {sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
      if (sd.from_sddl(L"O:SYG:SYD:P(A;;GA;;;SY)")) {
        sa.lpSecurityDescriptor = sd.p;
      }
      SetLastError(0);
      gen->section.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, VRIPC_SHM_SECTION_SIZE, nullptr));
      if (!gen->section || GetLastError() == ERROR_ALREADY_EXISTS) {
        fail = std::format("section err={}", GetLastError());
      } else {
        gen->shm = static_cast<vripc_shm_t *>(MapViewOfFile(gen->section.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, VRIPC_SHM_SECTION_SIZE));
        if (!gen->shm) {
          fail = std::format("map err={}", GetLastError());
        }
      }
      if (fail.empty()) {
        for (auto *e : {&gen->evt_trk, &gen->evt_frm}) {
          SetLastError(0);
          e->reset(CreateEventW(&sa, FALSE, FALSE, nullptr));
          if (!*e || GetLastError() == ERROR_ALREADY_EXISTS) {
            fail = std::format("event err={}", GetLastError());
            break;
          }
        }
      }
      if (!fail.empty()) {
        BOOST_LOG(error) << "[VIPLE-VR-IPC] handshake failed stage=create " << fail;
        verify_result_t r;
        r.reject = VRIPC_REJ_BUSY;
        r.reason = "server-resource " + fail;
        r.conn = std::move(c);
        reject_and_disconnect(r);
        return;
      }

      // header／config／pacing：WELCOME 之前寫好（config 在本 generation 內不再改）
      const int64_t now = now_qpc();
      auto *shm = gen->shm;
      std::memset(shm, 0, sizeof(vripc_shm_t));
      shm->header.magic = VRIPC_MAGIC_SHM;
      shm->header.abi_version = VRIPC_ABI_VERSION;
      shm->header.shm_size = VRIPC_SHM_SECTION_SIZE;
      shm->header.struct_size = (uint32_t) sizeof(vripc_shm_t);
      shm->header.generation = gen->generation;
      shm->header.qpc_frequency = qpf();
      shm->header.server_pid = GetCurrentProcessId();
      shm->header.armed = armed ? 1u : 0u;
      shm->header.server_state = VRIPC_SRV_INIT;
      vripc_store_release_u64(reinterpret_cast<uint64_t *>(&shm->header.server_heartbeat_qpc), (uint64_t) now);
      if (gen->has_config) {
        std::memcpy(&shm->config, &gen->config, sizeof(vripc_session_config_t));
      }
      c.gen = gen;
      c.armed_sent = armed;
      if (pacing && gen->has_config) {
        write_pacing(c, *pacing);
      } else {
        write_default_pacing(c);
      }

      // S3：最小權限複製進對方行程 → WELCOME（完整送達 = 所有權移交，之後只有對方能關）
      uint64_t r_shm = 0, r_trk = 0, r_frm = 0;
      HANDLE remote = nullptr;
      bool dup_ok = true;
      if (DuplicateHandle(GetCurrentProcess(), gen->section.get(), c.proc.get(), &remote, FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0)) {
        r_shm = (uint64_t) (uintptr_t) remote;
      } else {
        dup_ok = false;
      }
      if (dup_ok && DuplicateHandle(GetCurrentProcess(), gen->evt_trk.get(), c.proc.get(), &remote, SYNCHRONIZE, FALSE, 0)) {
        r_trk = (uint64_t) (uintptr_t) remote;
      } else {
        dup_ok = false;
      }
      if (dup_ok && DuplicateHandle(GetCurrentProcess(), gen->evt_frm.get(), c.proc.get(), &remote, EVENT_MODIFY_STATE, FALSE, 0)) {
        r_frm = (uint64_t) (uintptr_t) remote;
      } else {
        dup_ok = false;
      }
      const DWORD dup_err = dup_ok ? 0 : GetLastError();

      auto reclaim = [&]() {
        // 只有「寫入沒有完整送達」時才從遠端收回（sec-M1）
        for (uint64_t v : {r_shm, r_trk, r_frm}) {
          if (v) {
            DuplicateHandle(c.proc.get(), (HANDLE) (uintptr_t) v, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
          }
        }
      };

      if (!dup_ok) {
        reclaim();
        BOOST_LOG(error) << "[VIPLE-VR-IPC] handshake failed stage=duplicate err=" << dup_err;
        verify_result_t r;
        r.reject = VRIPC_REJ_BUSY;
        r.detail = dup_err;
        r.reason = std::format("duplicate err={}", dup_err);
        r.conn = std::move(c);
        reject_and_disconnect(r);
        return;
      }

      vripc_welcome_t w {};
      fill_hdr(w.hdr, VRIPC_MSG_WELCOME, sizeof w, ++c.out_seq);
      w.abi_version = VRIPC_ABI_VERSION;
      w.server_pid = GetCurrentProcessId();
      w.generation = gen->generation;
      w.shm_size = VRIPC_SHM_SECTION_SIZE;
      w.qpc_frequency = qpf();
      w.adapter_luid_low = gen->has_config ? gen->config.adapter_luid_low : 0;
      w.adapter_luid_high = gen->has_config ? gen->config.adapter_luid_high : 0;
      w.dup_shm_handle = r_shm;
      w.dup_evt_trk = r_trk;
      w.dup_evt_frm = r_frm;
      w.server_version_packed = server_version_packed();
      w.welcome_flags = dev ? VRIPC_WF_DEV_PACING : 0u;
      if (!write_msg(&w, sizeof w)) {
        reclaim();
        BOOST_LOG(warning) << "[VIPLE-VR-IPC] handshake failed stage=welcome-write err=" << last_io_error_;
        close_wait(kCloseWaitMs);
        DisconnectNamedPipe(pipe_.get());
        backoff_until_ = now_qpc() + ms_to_qpc(1000);
        return;
      }

      // 握手成功
      generation_counter_ = gen->generation;
      c.welcome_qpc = now_qpc();
      c.next_heartbeat = c.welcome_qpc + ms_to_qpc(kHeartbeatMs);
      c.log_window = c.haptic_window = c.state_log_window = c.timing_window = c.welcome_qpc;
      const bool is_peer = c.selftest_peer;
      const uint32_t dver = c.driver_version_packed;
      const std::string log_line = std::format(
        "[VIPLE-VR-IPC] handshake gen={} driver={} abi={} vrserver-pid={} iface={} ring={} luid={} identity={} caps={}",
        gen->generation,
        version_text(dver),
        VRIPC_ABI_VERSION,
        c.pid,
        c.iface.empty() ? "<none>" : c.iface,
        gen->has_config ? std::format("{}x{}", gen->config.packed_width, gen->config.packed_height) : std::string {"idle"},
        gen->has_config ? "set" : "n/a",
        is_peer ? "selftest-peer" : "vrserver",
        hex32(c.driver_caps)
      );
      conn_ = std::move(c);
      {
        std::lock_guard lk(mtx_);
        status_.connected = true;
        status_.peer_is_selftest = is_peer;
        status_.peer_pid = conn_->pid;
        status_.generation = gen->generation;
        status_.has_config = gen->has_config;
        status_.ready = false;
        status_.hmd_presenting = false;
        status_.hmd_added = false;
        status_.driver_version_packed = dver;
        status_.driver_caps = conn_->driver_caps;
        status_.driver_iface = conn_->iface;
        status_.peer_abi_version = VRIPC_ABI_VERSION;
        status_.handshakes++;
        counters_.handshakes_ok++;
        counters_.last_identity = is_peer ? 2u : 1u;
        cur_gen_ = gen;
        status_.phase = phase_e::welcomed;
        status_.driver_state = 0;
        status_.act_valid = false;
        status_.degraded_code = 0;
        status_.ring_corrupt = 0;
        status_.last_server_heartbeat_qpc = now;  // header 的第一個 heartbeat（WELCOME 之前寫）
        if (gen->has_config) {
          last_refresh_mhz_ = gen->config.refresh_mhz;
        }
      }
      if (gen->has_config) {
        std::lock_guard lk(trk_mtx_);
        trk_gen_ = gen;
        tracking_wanted_.store(true, std::memory_order_release);
      }
      BOOST_LOG(info) << log_line;
      arm_timer();
      emit({event_e::handshake, gen->generation, dver, is_peer ? 1u : 0u, {}});

      if (gen->has_config) {
        send_textures();
      }
      if (conn_) {
        start_read();
      }
    }

    void bridge_t::send_textures() {
      if (!conn_) {
        return;
      }
      auto &c = *conn_;
      auto &g = *c.gen;
      const luid_t luid {g.config.adapter_luid_low, g.config.adapter_luid_high};
      HRESULT hr = S_OK;
      if (!ensure_device(luid, hr) || !create_textures(g, hr)) {
        BOOST_LOG(error) << "[VIPLE-VR-IPC] textures-failed gen=" << g.generation << " hr=" << hex32((uint32_t) hr)
                         << " size=" << g.config.packed_width << 'x' << g.config.packed_height;
        {
          std::lock_guard lk(mtx_);
          counters_.textures_failed++;
        }
        emit({event_e::textures_failed, g.generation, (uint32_t) hr, 0, "create"});
        // server 端無法提供 ring：拆除並依 streak 退避，避免與 driver 形成緊密重連迴圈
        streak_ = std::min<uint32_t>(streak_ + 1, 1000);
        backoff_until_ = now_qpc() + ms_to_qpc(reject_backoff_sec(streak_) * 1000);
        teardown("textures-failed", true, VRIPC_BYE_PROTOCOL_ERROR, (uint32_t) hr);
        return;
      }

      std::array<uint64_t, VRIPC_TEX_COUNT + 2> remote {};
      std::array<HANDLE, VRIPC_TEX_COUNT + 2> src {};
      for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
        src[i] = g.tex_nt[i].get();
      }
      src[VRIPC_TEX_COUNT] = g.shared_fence_nt.get();
      src[VRIPC_TEX_COUNT + 1] = g.consumed_fence_nt.get();
      DWORD dup_err = 0;
      for (size_t i = 0; i < src.size(); ++i) {
        HANDLE r = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), src[i], c.proc.get(), &r, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
          dup_err = GetLastError();
          break;
        }
        remote[i] = (uint64_t) (uintptr_t) r;
      }
      auto reclaim = [&]() {
        for (uint64_t v : remote) {
          if (v) {
            DuplicateHandle(c.proc.get(), (HANDLE) (uintptr_t) v, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
          }
        }
      };
      if (dup_err) {
        reclaim();
        BOOST_LOG(error) << "[VIPLE-VR-IPC] textures-failed gen=" << g.generation << " stage=duplicate err=" << dup_err;
        {
          std::lock_guard lk(mtx_);
          counters_.textures_failed++;
        }
        emit({event_e::textures_failed, g.generation, dup_err, 0, "duplicate"});
        teardown("textures-duplicate-failed", true, VRIPC_BYE_PROTOCOL_ERROR, dup_err);
        return;
      }

      vripc_textures_t t {};
      fill_hdr(t.hdr, VRIPC_MSG_TEXTURES, sizeof t, ++c.out_seq);
      t.generation = g.generation;
      t.adapter_luid_low = luid.low;
      t.adapter_luid_high = luid.high;
      t.width = g.config.packed_width;
      t.height = g.config.packed_height;
      t.dxgi_format = VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM;
      t.tex_count = VRIPC_TEX_COUNT;
      for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
        t.tex_handle[i] = remote[i];
      }
      t.shared_fence_handle = remote[VRIPC_TEX_COUNT];
      t.consumed_fence_handle = remote[VRIPC_TEX_COUNT + 1];
      t.shared_fence_initial = 0;
      if (!write_msg(&t, sizeof t)) {
        reclaim();
        teardown(std::format("textures-write-failed err={}", last_io_error_), false, 0, 0);
        return;
      }
      c.textures_sent = true;
      c.ready_deadline = now_qpc() + ms_to_qpc(kReadyTimeoutMs);
    }

    // ── 穩態訊息（§B.4 之後）────────────────────────────────────────────────
    void bridge_t::handle_message(const uint8_t *buf, DWORD n) {
      if (!conn_) {
        return;
      }
      auto &c = *conn_;
      vripc_msg_hdr_t hdr {};
      if (n < sizeof hdr) {
        protocol_error(std::format("short-message bytes={}", n), VRIPC_REJ_BAD_MESSAGE);
        return;
      }
      std::memcpy(&hdr, buf, sizeof hdr);
      if (hdr.magic != VRIPC_MAGIC_PIPE || hdr.size != n) {
        protocol_error(std::format("bad-header bytes={}", n), VRIPC_REJ_BAD_MESSAGE);
        return;
      }
      if (hdr.seq <= c.in_seq) {
        protocol_error(std::format("seq {}<={}", hdr.seq, c.in_seq), VRIPC_REJ_BAD_MESSAGE);
        return;
      }
      c.in_seq = hdr.seq;

      switch (hdr.type) {
        case VRIPC_MSG_READY:
          {
            if (n != sizeof(vripc_ready_t)) {
              protocol_error("ready-size", VRIPC_REJ_BAD_MESSAGE);
              return;
            }
            vripc_ready_t m {};
            std::memcpy(&m, buf, sizeof m);
            if (m.generation != c.gen->generation) {
              protocol_error("ready-generation", VRIPC_REJ_GENERATION);
              return;
            }
            if (!c.textures_sent || c.ready) {
              protocol_error("ready-unexpected", VRIPC_REJ_BAD_MESSAGE);
              return;
            }
            c.ready = true;
            auto &g = *c.gen;
            *(volatile uint32_t *) &g.shm->header.server_state = VRIPC_SRV_READY;

            auto fs = std::make_shared<frame_source_t>();
            fs->generation = g.generation;
            fs->luid = luid_t {g.config.adapter_luid_low, g.config.adapter_luid_high};
            fs->width = g.config.packed_width;
            fs->height = g.config.packed_height;
            fs->dxgi_format = VRIPC_DXGI_FORMAT_B8G8R8A8_UNORM;
            fs->refresh_mhz = g.config.refresh_mhz;
            fs->layout_epoch = g.config.layout_epoch;
            fs->space_epoch = g.config.space_epoch;
            fs->shared_fence_initial = 0;
            for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
              fs->tex_nt[i] = g.tex_nt[i].get();
            }
            fs->shared_fence_nt = g.shared_fence_nt.get();
            fs->consumed_fence_nt = g.consumed_fence_nt.get();
            fs->evt_frm = g.evt_frm.get();
            fs->impl = c.gen;
            const double ready_ms = qpc_to_ms(now_qpc() - c.welcome_qpc);
            {
              std::lock_guard lk(mtx_);
              source_ = fs;
              status_.ready = true;
              status_.phase = phase_e::ready;
            }
            BOOST_LOG(info) << "[VIPLE-VR-IPC] ready gen=" << g.generation << " ring=" << g.config.packed_width << 'x' << g.config.packed_height
                            << " readyMs=" << std::format("{:.1f}", ready_ms);
            emit({event_e::ready, g.generation, 0, 0, {}});
            return;
          }
        case VRIPC_MSG_STATE:
          {
            if (n != sizeof(vripc_state_t)) {
              protocol_error("state-size", VRIPC_REJ_BAD_MESSAGE);
              return;
            }
            vripc_state_t s {};
            std::memcpy(&s, buf, sizeof s);
            if (s.generation != c.gen->generation) {
              protocol_error("state-generation", VRIPC_REJ_GENERATION);
              return;
            }
            if (s.kind < 0x81u || s.kind > 0xBFu) {
              protocol_error(std::format("state-kind {}", hex32(s.kind)), VRIPC_REJ_BAD_MESSAGE);
              return;
            }
            handle_state(s);
            return;
          }
        case VRIPC_MSG_BYE:
          {
            if (n != sizeof(vripc_bye_t)) {
              protocol_error("bye-size", VRIPC_REJ_BAD_MESSAGE);
              return;
            }
            vripc_bye_t b {};
            std::memcpy(&b, buf, sizeof b);
            teardown(std::format("driver-bye reason={} detail={}", b.reason, b.detail), false, 0, 0);
            return;
          }
        case VRIPC_MSG_REJECT:
          {
            if (n != sizeof(vripc_reject_t)) {
              protocol_error("reject-size", VRIPC_REJ_BAD_MESSAGE);
              return;
            }
            vripc_reject_t m {};
            std::memcpy(&m, buf, sizeof m);
            if (m.reason == VRIPC_REJ_ADAPTER_MISMATCH || m.reason == VRIPC_REJ_TEXTURE_INVALID) {
              emit({event_e::textures_failed, c.gen->generation, m.reason, m.detail, "driver-reject"});
            }
            teardown(std::format("driver-reject reason={} detail={}", m.reason, hex32(m.detail)), false, 0, 0);
            return;
          }
        default:
          protocol_error(std::format("unexpected-type {}", hdr.type), VRIPC_REJ_BAD_MESSAGE);
          return;
      }
    }

    void bridge_t::handle_state(const vripc_state_t &s) {
      auto &c = *conn_;
      switch (s.kind) {
        case VRIPC_ST_HMD_ADDED:
          {
            std::lock_guard lk(mtx_);
            status_.hmd_added = true;
            break;
          }
        case VRIPC_ST_HMD_PRESENTING:
          {
            std::lock_guard lk(mtx_);
            status_.hmd_presenting = true;
            break;
          }
        case VRIPC_ST_HMD_STANDBY:
          {
            std::lock_guard lk(mtx_);
            status_.hmd_presenting = false;
            break;
          }
        case VRIPC_ST_DEGRADED:
          {
            std::lock_guard lk(mtx_);
            status_.degraded_code = s.arg;
            break;
          }
        case VRIPC_ST_HMD_ACTIVATED:
          snapshot_driver(now_qpc());  // act_* 此時已寫好
          if (!conn_) {
            return;
          }
          break;
        default:
          break;
      }

      const int64_t now = now_qpc();
      if (now - c.state_log_window >= qpf()) {
        c.state_log_window = now;
        c.state_logs = 0;
      }
      if (c.state_logs++ < kStateLogPerSec) {
        BOOST_LOG(info) << "[VIPLE-VR-IPC] driver-state gen=" << s.generation << " kind=" << state_kind_name(s.kind)
                        << " arg=" << (s.kind == VRIPC_ST_DEVICE_LOST || s.kind == VRIPC_ST_DEGRADED ? hex32(s.arg) : std::to_string(s.arg));
      }
      emit({event_e::driver_state, s.generation, s.kind, s.arg, state_kind_name(s.kind)});
    }

    void bridge_t::send_state(uint32_t kind, uint32_t arg) {
      if (!conn_) {
        return;
      }
      vripc_state_t m {};
      fill_hdr(m.hdr, VRIPC_MSG_STATE, sizeof m, ++conn_->out_seq);
      m.generation = conn_->gen->generation;
      m.kind = kind;
      m.arg = arg;
      {
        // 寫入之前計數：寫入卡住（對方不讀 pipe）時，selftest 以最後一次計數的時間當「開始卡住」的時間點
        std::lock_guard lk(mtx_);
        counters_.states_sent++;
      }
      send(&m, sizeof m);
    }

    void bridge_t::protocol_error(const std::string &reason, uint32_t detail) {
      teardown("protocol-error " + reason, true, VRIPC_BYE_PROTOCOL_ERROR, detail);
    }

    void bridge_t::teardown(const std::string &reason, bool send_bye, uint32_t bye_reason, uint32_t bye_detail, bool wait_close) {
      if (!conn_ || in_teardown_) {
        return;
      }
      in_teardown_ = true;
      {
        // 對外狀態一進 TEARDOWN 就變成「未連線」（selftest 的 kill→idle ≤ 100 ms 以它為準；之後的 close_wait
        // 最多 200 ms 只是等對方關閉，期間 driver 重連會拿到 PIPE_BUSY 並自行重試）
        std::lock_guard lk(mtx_);
        status_.phase = phase_e::teardown;
        status_.connected = false;
        status_.ready = false;
      }
      auto &c = *conn_;
      const uint64_t gen = c.gen ? c.gen->generation : 0;
      if (c.gen && c.gen->shm) {
        *(volatile uint32_t *) &c.gen->shm->header.server_state = VRIPC_SRV_CLOSING;
      }
      if (send_bye) {
        vripc_bye_t b {};
        fill_hdr(b.hdr, VRIPC_MSG_BYE, sizeof b, ++c.out_seq);
        b.reason = bye_reason;
        b.detail = bye_detail;
        {
          std::lock_guard lk(mtx_);
          counters_.byes_sent[std::min<uint32_t>(bye_reason, std::size(counters_.byes_sent) - 1)]++;
        }
        if (!write_msg(&b, sizeof b)) {
          wait_close = false;  // BYE 都送不出去（對方不讀）：不再等它關閉
        }
      }
      if (wait_close) {
        close_wait(kCloseWaitMs);
      } else {
        cancel_read();
      }
      DisconnectNamedPipe(pipe_.get());
      disarm_timer();
      {
        std::lock_guard lk(trk_mtx_);
        if (trk_gen_ == c.gen) {
          trk_gen_.reset();
        }
        tracking_wanted_.store(false, std::memory_order_release);
      }
      if (c.gen) {
        c.gen->alive.store(false, std::memory_order_release);
      }
      {
        std::lock_guard lk(mtx_);
        source_.reset();
        cur_gen_.reset();
        status_.connected = false;
        status_.ready = false;
        status_.hmd_presenting = false;
        status_.peer_is_selftest = false;
        status_.teardowns++;
        counters_.teardowns++;
        status_.last_teardown_reason = reason;
      }
      BOOST_LOG(info) << "[VIPLE-VR-IPC] teardown gen=" << gen << " reason=" << reason << (send_bye ? std::format(" bye={}", bye_reason) : std::string {});
      // 已移交給對方的 handle 一律不從遠端關（§B.1 所有權）；自己的 ring／fence／event／section 參照與 view
      // 隨 gen_t 的最後一個 shared_ptr 釋放（frame_source_t 可能還持有，直到消費端放掉）
      conn_.reset();
      in_teardown_ = false;
      emit({event_e::teardown, gen, 0, 0, reason});
    }

    // ── 命令與週期工作 ──────────────────────────────────────────────────────
    void bridge_t::process_commands() {
      commands_t cmd;
      std::optional<vripc_session_config_t> cfg;
      bool armed = false;
      bool dev = false;
      {
        std::lock_guard lk(mtx_);
        cmd = std::exchange(cmds_, commands_t {});
        if (cmd.config_changed) {
          cfg = desired_config_;
        }
        armed = desired_armed_;
        dev = dev_mode_;
      }
      if (!conn_) {
        return;
      }
      if (cmd.teardown && cmd.teardown->first == conn_->gen->generation) {
        teardown("requested", true, cmd.teardown->second, 0);  // 後綴會補上 bye=<reason>
        return;
      }
      if (cmd.config_changed) {
        const auto &g = *conn_->gen;
        const bool differs = g.has_config != cfg.has_value() || (cfg && std::memcmp(&g.config, &*cfg, sizeof(vripc_session_config_t)) != 0);
        if (differs) {
          // generation 不變式：config 改變 = 新 generation；driver 立刻重連（不退避）
          teardown("reconfig", true, VRIPC_BYE_RECONFIG, 0);
          return;
        }
      }
      if (armed != conn_->armed_sent) {
        *(volatile uint32_t *) &conn_->gen->shm->header.armed = armed ? 1u : 0u;
        conn_->armed_sent = armed;
        send_state(armed ? VRIPC_ST_ARM : VRIPC_ST_DISARM, 0);
        if (!conn_) {
          return;
        }
      }
      if (cmd.quit) {
        send_state(VRIPC_ST_REQUEST_QUIT, 0);
        if (!conn_) {
          return;
        }
      }
      if (cmd.v2p && dev) {
        send_state(VRIPC_ST_DEV_SET_V2P, *cmd.v2p);
        if (!conn_) {
          return;
        }
      }
      if (cmd.pacing) {
        write_pacing(*conn_, *cmd.pacing);
      }
    }

    bool bridge_t::snapshot_driver(int64_t now) {
      auto &c = *conn_;
      // §B.8 第 1、9 條：整份複製一次，之後只用本地副本
      vripc_driver_status_t d {};
      std::memcpy(&d, (const void *) &c.gen->shm->driver, sizeof d);
      c.last_driver = d;

      const bool state_ok = d.driver_state == VRIPC_DRV_MAPPED || (d.driver_state >= VRIPC_DRV_READY && d.driver_state <= VRIPC_DRV_DEGRADED);
      const bool hb_ok = d.driver_heartbeat_qpc >= now - 10 * qpf() && d.driver_heartbeat_qpc <= now + qpf();
      const bool act_ok = (d.act_refresh_mhz == 0 || (d.act_refresh_mhz >= 60000 && d.act_refresh_mhz <= 240000)) && d.act_eye_width <= 8192 && d.act_eye_height <= 8192;
      char sys[sizeof d.other_hmd_system + 1] {};
      std::memcpy(sys, d.other_hmd_system, sizeof d.other_hmd_system);
      // 0 = driver 還沒寫（剛 MAPPED 前），不算不合格；其餘不在允許集合／範圍內的欄位都計數並當成 UNKNOWN
      bool str_ok = std::memchr(d.other_hmd_system, 0, sizeof d.other_hmd_system) != nullptr;
      for (size_t i = 0; str_ok && i < sizeof d.other_hmd_system && sys[i] != '\0'; ++i) {
        str_ok = (unsigned char) sys[i] >= 0x20 && (unsigned char) sys[i] <= 0x7E;
      }
      const bool invalid = (d.driver_state != 0 && !state_ok) || d.other_hmd > VRIPC_OTHER_HMD_OURS || !str_ok ||
                           (d.driver_heartbeat_qpc != 0 && !hb_ok) || !act_ok;
      {
        std::lock_guard lk(mtx_);
        if (invalid) {
          counters_.driver_status_invalid++;
        }
        status_.driver_state = state_ok ? d.driver_state : 0;
        status_.other_hmd = d.other_hmd <= VRIPC_OTHER_HMD_OURS ? d.other_hmd : VRIPC_OTHER_HMD_UNKNOWN;
        status_.other_hmd_system = sanitize(sys, sizeof d.other_hmd_system);
        if (hb_ok) {
          status_.last_driver_heartbeat_qpc = d.driver_heartbeat_qpc;
        }
        status_.act_valid = act_ok;
        status_.act_refresh_mhz = act_ok ? d.act_refresh_mhz : 0;
        status_.act_eye_w = act_ok ? d.act_eye_width : 0;
        status_.act_eye_h = act_ok ? d.act_eye_height : 0;
        status_.act_luid = act_ok ? luid_t {d.act_luid_low, d.act_luid_high} : luid_t {};
        if (d.degraded_code) {
          status_.degraded_code = d.degraded_code;
        }
        status_.ring_corrupt = c.ring_corrupt;
      }

      // driver heartbeat > 2 s 而 pipe 仍開 → hung（第一次 heartbeat 給 3 s）
      const bool hung = hb_ok ? (now - d.driver_heartbeat_qpc > ms_to_qpc(kDriverHungMs)) : (now - c.welcome_qpc > ms_to_qpc(kDriverFirstBeatMs));
      if (hung) {
        const uint64_t gen = c.gen->generation;
        {
          std::lock_guard lk(mtx_);
          counters_.driver_hung++;
        }
        BOOST_LOG(warning) << "[VIPLE-VR-IPC] driver-hung gen=" << gen << " heartbeat=" << (hb_ok ? "stale" : "none");
        emit({event_e::driver_hung, gen, 0, 0, hb_ok ? "stale" : "none"});
        teardown("driver-hung", true, VRIPC_BYE_PROTOCOL_ERROR, VRIPC_REJ_TIMEOUT);
        return false;
      }
      return true;
    }

    template<class T, size_t N, class F>
    uint32_t drain_spsc(uint64_t *head, uint64_t *tail, const T (&items)[N], uint64_t &local_tail, uint32_t &corrupt, F &&fn) {
      const uint64_t h = vripc_load_acquire_u64(head);
      bool moved = false;
      if (h < local_tail) {
        ++corrupt;  // 倒退
        local_tail = h;
        moved = true;
      } else if (h - local_tail > N) {
        ++corrupt;  // 越界：只取最後 N 筆
        local_tail = h - N;
        moved = true;
      }
      uint32_t count = 0;
      while (local_tail < h) {
        T item;
        std::memcpy(&item, (const void *) &items[local_tail % N], sizeof item);
        ++local_tail;
        ++count;
        fn(item);
      }
      if (count || moved) {
        vripc_store_release_u64(tail, local_tail);
      }
      return count;
    }

    void bridge_t::drain_rings(int64_t now) {
      auto &c = *conn_;
      auto *shm = c.gen->shm;
      // 快速路徑（每 2 ms 一次）：三個 head 都沒動就不取鎖、不複製 sink
      const bool any = vripc_load_acquire_u64(&shm->haptic.head) != c.haptic_tail || vripc_load_acquire_u64(&shm->timing.head) != c.timing_tail ||
                       vripc_load_acquire_u64(&shm->log.head) != c.log_tail;
      haptic_cb hsink;
      timing_cb tsink;
      if (any) {
        std::lock_guard lk(mtx_);
        hsink = haptic_sink_;
        tsink = timing_sink_;
      }
      uint64_t haptic_rx = 0, haptic_dropped = 0, log_lines = 0, log_supp = 0, timing_rx = 0;
      const uint32_t corrupt_before = c.ring_corrupt;

      if (now - c.haptic_window >= qpf()) {
        c.haptic_window = now;
        c.haptic_in_window[0] = c.haptic_in_window[1] = 0;
      }
      drain_spsc(&shm->haptic.head, &shm->haptic.tail, shm->haptic.evt, c.haptic_tail, c.ring_corrupt, [&](vripc_haptic_evt_t e) {
        if (e.device != 1 && e.device != 2) {
          ++haptic_dropped;
          return;
        }
        if (c.haptic_in_window[e.device - 1]++ >= kHapticPerSecPerHand) {
          ++haptic_dropped;
          return;
        }
        e.duration_us = std::min<uint32_t>(e.duration_us, 2'000'000u);
        e.frequency_hz = std::isfinite(e.frequency_hz) ? std::clamp(e.frequency_hz, 0.0f, 1000.0f) : 0.0f;
        e.amplitude = std::isfinite(e.amplitude) ? std::clamp(e.amplitude, 0.0f, 1.0f) : 0.0f;
        ++haptic_rx;
        if (hsink) {
          hsink(e);
        }
      });

      drain_spsc(&shm->timing.head, &shm->timing.tail, shm->timing.rec, c.timing_tail, c.ring_corrupt, [&](const vripc_timing_rec_t &r) {
        ++timing_rx;
        ++c.timing_total;
        c.timing_counts[r.event < 16 ? r.event : 0]++;
        if (tsink) {
          tsink(r);
        }
      });

      if (now - c.log_window >= qpf()) {
        if (c.log_suppressed) {
          BOOST_LOG(info) << "[VIPLE-VR-DRV] (server suppressed " << c.log_suppressed << " lines)";
        }
        c.log_window = now;
        c.log_in_window = 0;
        c.log_suppressed = 0;
      }
      drain_spsc(&shm->log.head, &shm->log.tail, shm->log.line, c.log_tail, c.ring_corrupt, [&](const vripc_log_line_t &l) {
        if (c.log_in_window >= kDriverLogPerSec) {
          ++c.log_suppressed;
          ++log_supp;
          return;
        }
        ++c.log_in_window;
        ++log_lines;
        // §B.8 第 8 條：level 夾到 0–2、len 夾到 < 240、非可列印字元與 '[' 換成 '?'、前綴由 server 加
        const size_t len = std::min<size_t>(l.len, VRIPC_LOG_TEXT - 1);
        const std::string text = sanitize(l.text, len);
        switch (std::min<uint32_t>(l.level, 2)) {
          case 0:
            BOOST_LOG(info) << "[VIPLE-VR-DRV] " << text;
            break;
          case 1:
            BOOST_LOG(warning) << "[VIPLE-VR-DRV] " << text;
            break;
          default:
            BOOST_LOG(error) << "[VIPLE-VR-DRV] " << text;
            break;
        }
      });

      if (haptic_rx || haptic_dropped || log_lines || log_supp || timing_rx || c.ring_corrupt != corrupt_before) {
        std::lock_guard lk(mtx_);
        counters_.ring_corrupt += c.ring_corrupt - corrupt_before;
        status_.haptic_rx += haptic_rx;
        status_.haptic_dropped += haptic_dropped;
        status_.drv_log_lines += log_lines;
        status_.drv_log_suppressed += log_supp;
        status_.timing_rx += timing_rx;
        status_.ring_corrupt = c.ring_corrupt;
      }

      // timing 10 s 摘要（有 timing 記錄才印）
      if (now - c.timing_window >= 10 * qpf()) {
        if (c.timing_total) {
          const auto &d = c.last_driver;
          const auto *k = c.timing_counts;
          BOOST_LOG(info) << "[VIPLE-VR-DRV] timing 10s: recs=" << c.timing_total << " vsync=" << k[VRIPC_TE_VSYNC_VIRTUAL]
                          << " submit=" << k[VRIPC_TE_SUBMIT_LAYER] << " present=" << k[VRIPC_TE_PRESENT_EXIT]
                          << " postpresent=" << k[VRIPC_TE_POSTPRESENT_EXIT] << " compose=" << k[VRIPC_TE_COMPOSE_DONE]
                          << " pose=" << k[VRIPC_TE_POSE_UPDATED] << " stale=" << k[VRIPC_TE_STALE] << " snap=" << k[VRIPC_TE_PHASE_SNAP]
                          << " pacingRejected=" << k[VRIPC_TE_PACING_REJECTED] << " nextIndex=" << k[VRIPC_TE_NEXT_INDEX]
                          << " | drv presented=" << d.frames_presented << " composited=" << d.frames_composited
                          << " noslot=" << d.drop_noslot << " acqTimeout=" << d.drop_acquire_timeout << " fmt=" << d.drop_format
                          << " staleCnt=" << d.stale_pose_count << " oor=" << d.outofrange_count << " posehistMiss=" << d.posehist_miss;
        }
        c.timing_window = now;
        c.timing_total = 0;
        std::fill(std::begin(c.timing_counts), std::end(c.timing_counts), 0);
      }

      if (c.ring_corrupt >= kRingCorruptLimit) {
        protocol_error(std::format("ring-corrupt count={}", c.ring_corrupt), 0);
      }
    }

    void bridge_t::periodic(int64_t now) {
      auto &c = *conn_;
      if (c.textures_sent && !c.ready && now > c.ready_deadline) {
        teardown("ready-timeout", true, VRIPC_BYE_PROTOCOL_ERROR, VRIPC_REJ_TIMEOUT);
        return;
      }
      if (now >= c.next_heartbeat) {
        c.next_heartbeat = now + ms_to_qpc(kHeartbeatMs);
        vripc_store_release_u64(reinterpret_cast<uint64_t *>(&c.gen->shm->header.server_heartbeat_qpc), (uint64_t) now);
        {
          // 對外快照（selftest T2.never-read 量「bridge heartbeat 不中斷」；每 100 ms 一次）
          std::lock_guard lk(mtx_);
          status_.last_server_heartbeat_qpc = now;
        }
        if (!snapshot_driver(now)) {
          return;
        }
      }
      drain_rings(now);
    }

    DWORD bridge_t::compute_timeout(int64_t now, int64_t next_pipe_try, int64_t next_user_poll) const {
      if (conn_) {
        return timer_armed_ ? 100 : 2;
      }
      int64_t next = now + ms_to_qpc(1000);
      if (!pipe_) {
        next = std::min(next, next_pipe_try);
      } else {
        next = std::min(next, next_user_poll);
        if (user_ && !listening_) {
          next = std::min(next, backoff_until_);
        }
      }
      return std::max<DWORD>(1, ceil_ms(next - now));
    }

    void bridge_t::thread_main() {
      platf::set_thread_name("vr_bridge");
      int64_t next_pipe_try = 0;
      int64_t next_user_poll = 0;
      for (;;) {
        int64_t now = now_qpc();
        if (!pipe_ && now >= next_pipe_try) {
          if (create_pipe()) {
            next_user_poll = 0;
          } else {
            next_pipe_try = now + ms_to_qpc(kPipeRetryMs);
          }
        }
        if (pipe_ && (user_changed_.exchange(false) || now_qpc() >= next_user_poll)) {
          refresh_user();
          next_user_poll = now_qpc() + ms_to_qpc(user_ ? kUserPollMs : kUserPollNoUserMs);
        }
        process_commands();
        now = now_qpc();
        if (pipe_ && !conn_ && !listening_) {
          if (!user_) {
            set_phase(phase_e::no_user);
          } else if (now >= backoff_until_) {
            start_listen();
          } else {
            set_phase(phase_e::backoff);
          }
        }
        if (conn_) {
          periodic(now_qpc());
        }

        HANDLE hs[4];
        DWORD n = 0;
        hs[n++] = ev_stop_.get();
        hs[n++] = ev_cmd_.get();
        DWORD read_idx = MAXDWORD;
        if (listening_ || (conn_ && read_pending_)) {
          read_idx = n;
          hs[n++] = ev_read_.get();
        }
        if (conn_ && timer_armed_) {
          hs[n++] = timer_.get();
        }
        const DWORD w = WaitForMultipleObjects(n, hs, FALSE, compute_timeout(now_qpc(), next_pipe_try, next_user_poll));
        if (w == WAIT_OBJECT_0) {
          break;
        }
        if (read_idx != MAXDWORD && w == WAIT_OBJECT_0 + read_idx) {
          on_read_event();
        }
      }
      shutdown_all();
    }

    void bridge_t::shutdown_all() {
      if (conn_) {
        teardown("server-shutdown", true, VRIPC_BYE_SERVER_SHUTDOWN, 0);
      }
      stop_listen();
      {
        std::lock_guard lk(pipe_mtx_);
        pipe_.reset();
      }
      user_.reset();
      disarm_timer();
      bdev5_.reset();
      bdev_.reset();
      std::lock_guard lk(mtx_);
      status_.user_present = false;
      status_.pipe_reason = "not-started";
      user_logon_copy_.clear();
      BOOST_LOG(info) << "[VIPLE-VR-IPC] stopped";
    }

    // ── T0：pipe SD 自檢 ────────────────────────────────────────────────────
    pipe_sd_report_t bridge_t::check_pipe_security(bool roundtrip) {
      pipe_sd_report_t r;
      r.supported = true;
      sid_t logon;
      {
        std::lock_guard lk(mtx_);
        r.pipe_reason = status_.pipe_reason;
        r.self_session = status_.self_session;
        logon = user_logon_copy_;
      }
      r.console_session = WTSGetActiveConsoleSessionId();
      std::lock_guard lk(pipe_mtx_);
      if (!pipe_) {
        return r;
      }
      r.pipe_exists = true;
      PSID owner = nullptr;
      PACL dacl = nullptr;
      PACL sacl = nullptr;
      PSECURITY_DESCRIPTOR sd = nullptr;
      const DWORD e = GetSecurityInfo(pipe_.get(), SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION, &owner, nullptr, &dacl, &sacl, &sd);
      if (e != ERROR_SUCCESS) {
        r.query_error = e;
        return r;
      }
      const auto sys = well_known_sid(WinLocalSystemSid);
      r.owner_is_system = sid_equal(owner, sys);
      SECURITY_DESCRIPTOR_CONTROL ctl = 0;
      DWORD rev = 0;
      if (GetSecurityDescriptorControl(sd, &ctl, &rev)) {
        r.dacl_protected = (ctl & SE_DACL_PROTECTED) != 0;
      }
      if (dacl) {
        r.ace_count = dacl->AceCount;
        for (DWORD i = 0; i < dacl->AceCount; ++i) {
          void *p = nullptr;
          if (!GetAce(dacl, i, &p)) {
            continue;
          }
          const auto *h = static_cast<const ACE_HEADER *>(p);
          if (h->AceType != ACCESS_ALLOWED_ACE_TYPE) {
            continue;
          }
          auto *a = static_cast<ACCESS_ALLOWED_ACE *>(p);
          PSID s = reinterpret_cast<PSID>(&a->SidStart);
          if (sid_equal(s, sys)) {
            r.system_ace = true;
            r.system_ace_mask = a->Mask;
          } else {
            r.user_ace = true;
            r.user_ace_mask = a->Mask;
            r.user_ace_is_logon_sid = is_logon_sid(s);
            r.user_ace_matches_console = sid_equal(s, logon);
          }
        }
      }
      if (sacl) {
        for (DWORD i = 0; i < sacl->AceCount; ++i) {
          void *p = nullptr;
          if (!GetAce(sacl, i, &p)) {
            continue;
          }
          const auto *h = static_cast<const ACE_HEADER *>(p);
          if (h->AceType != SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
            continue;
          }
          auto *l = static_cast<SYSTEM_MANDATORY_LABEL_ACE *>(p);
          r.label_present = true;
          r.label_policy = l->Mask;
          r.label_rid = sid_rid(reinterpret_cast<PSID>(&l->SidStart));
        }
      }
      if (roundtrip) {
        // 以目前的 DACL 原樣寫回：證明 open mode 帶了 WRITE_DAC，而且不改變狀態
        r.roundtrip_attempted = true;
        r.roundtrip_error = SetSecurityInfo(pipe_.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        r.roundtrip_ok = r.roundtrip_error == ERROR_SUCCESS;
      }
      LocalFree(sd);
      return r;
    }

    // ── T2 消費者 ───────────────────────────────────────────────────────────
    struct consumer_t {
      luid_t luid;
      bool have_device = false;
      com_ptr<ID3D11Device> dev;
      com_ptr<ID3D11DeviceContext> ctx;
      com_ptr<ID3D11Device1> dev1;
      com_ptr<ID3D11Device5> dev5;
      com_ptr<ID3D11DeviceContext4> ctx4;
      com_ptr<ID3D11Texture2D> tex[VRIPC_TEX_COUNT];
      com_ptr<ID3D11Fence> shared_fence;
      com_ptr<ID3D11Fence> consumed_fence;
      com_ptr<ID3D11Texture2D> scratch;
      com_ptr<ID3D11Query> query;

      /// 以 frame_source 的 NT handle 在自己的 device 上開 ring／fence（display_vr_t 將走同一條路徑）
      std::string open(const frame_source_t &src) {
        if (!have_device || !(luid == src.luid) || dev->GetDeviceRemovedReason() != S_OK) {
          ctx4.reset();
          dev5.reset();
          dev1.reset();
          ctx.reset();
          dev.reset();
          have_device = false;
          HRESULT hr = create_device_on_luid(src.luid, dev, &ctx);
          if (FAILED(hr)) {
            return "create-device " + hex32((uint32_t) hr);
          }
          if (FAILED(hr = dev->QueryInterface(__uuidof(ID3D11Device1), dev1.put_void())) ||
              FAILED(hr = dev->QueryInterface(__uuidof(ID3D11Device5), dev5.put_void())) ||
              FAILED(hr = ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), ctx4.put_void()))) {
            return "query-interface " + hex32((uint32_t) hr);
          }
          luid = src.luid;
          have_device = true;
        }
        for (uint32_t i = 0; i < VRIPC_TEX_COUNT; ++i) {
          const HRESULT hr = dev1->OpenSharedResource1(static_cast<HANDLE>(src.tex_nt[i]), __uuidof(ID3D11Texture2D), tex[i].put_void());
          if (FAILED(hr)) {
            return std::format("open-texture[{}] {}", i, hex32((uint32_t) hr));
          }
        }
        HRESULT hr = dev5->OpenSharedFence(static_cast<HANDLE>(src.shared_fence_nt), __uuidof(ID3D11Fence), shared_fence.put_void());
        if (FAILED(hr)) {
          return "open-shared-fence " + hex32((uint32_t) hr);
        }
        hr = dev5->OpenSharedFence(static_cast<HANDLE>(src.consumed_fence_nt), __uuidof(ID3D11Fence), consumed_fence.put_void());
        if (FAILED(hr)) {
          return "open-consumed-fence " + hex32((uint32_t) hr);
        }
        D3D11_TEXTURE2D_DESC d {};
        tex[0]->GetDesc(&d);
        if (d.Width != src.width || d.Height != src.height || d.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
          return std::format("desc-mismatch {}x{} fmt={}", d.Width, d.Height, (uint32_t) d.Format);
        }
        d.BindFlags = 0;
        d.MiscFlags = 0;
        hr = dev->CreateTexture2D(&d, nullptr, scratch.put());
        if (FAILED(hr)) {
          return "scratch " + hex32((uint32_t) hr);
        }
        D3D11_QUERY_DESC qd {D3D11_QUERY_EVENT, 0};
        hr = dev->CreateQuery(&qd, query.put());
        if (FAILED(hr)) {
          return "query " + hex32((uint32_t) hr);
        }
        return {};
      }
    };

    void percentiles(std::vector<double> v, double &p50, double &p95, double &mx) {
      if (v.empty()) {
        p50 = p95 = mx = 0;
        return;
      }
      std::sort(v.begin(), v.end());
      auto at = [&](double q) {
        const auto i = (size_t) std::min<double>((double) v.size() - 1, std::floor(q * (double) (v.size() - 1) + 0.5));
        return v[i];
      };
      p50 = at(0.5);
      p95 = at(0.95);
      mx = v.back();
    }

    std::mutex g_consume_mtx;
  }  // namespace

  // ── frame_source_t／frame_reader_t／fence_waiter_t ─────────────────────────
  bool frame_source_t::alive() const {
    const auto g = gen_of(*this);
    return g && g->alive.load(std::memory_order_acquire);
  }

  frame_reader_t::frame_reader_t(std::shared_ptr<frame_source_t> src):
      src_(std::move(src)) {
    if (src_) {
      last_fence_ = src_->shared_fence_initial;
    }
  }

  frame_reader_t::result_e frame_reader_t::read_latest(vripc_frame_desc_t &out) {
    last_skipped_ = 0;
    if (!src_) {
      return result_e::none;
    }
    const auto g = gen_of(*src_);
    if (!g || !g->shm) {
      return result_e::none;
    }
    auto &ring = g->shm->frames;

    // §B.3 讀取方：只要最新一筆，最多試 3 次（寫入方不可信，不可無限重試）
    vripc_frame_desc_t local {};
    bool got = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
      const uint64_t w = vripc_load_acquire_u64(&ring.write_index);
      if (w == 0 || w == last_write_index_) {
        return result_e::none;
      }
      const uint64_t idx = w - 1;
      auto *s = &ring.slot[idx % VRIPC_FRM_SLOTS];
      const uint64_t s1 = vripc_load_acquire_u64(&s->seq);
      if (s1 != 2 * idx + 2) {
        continue;
      }
      std::memcpy(&local, (const void *) s, sizeof local);
      VRIPC_COMPILER_BARRIER();
      const uint64_t s2 = vripc_load_acquire_u64(&s->seq);
      if (s1 == s2 && local.seq == s1) {
        last_write_index_ = w;
        got = true;
        break;
      }
    }
    if (!got) {
      ++torn_total;
      return result_e::torn;
    }

    // 換消費者後的第一筆可能是上一個消費者已經放回的幀（fence ≤ 基準）：當成沒有新資料，不算不合格
    if (!have_last_frame_ && local.generation == src_->generation && local.fence_value <= last_fence_) {
      return result_e::none;
    }

    // §B.8：只驗本地副本
    frame_check_ctx_t ctx;
    ctx.generation = src_->generation;
    ctx.have_last_frame = have_last_frame_;
    ctx.last_frame_id = last_frame_id_;
    ctx.last_fence = last_fence_;
    ctx.now_qpc = now_qpc();
    ctx.qpc_frequency = qpf();
    ctx.layout_epoch = src_->layout_epoch;
    ctx.space_epoch = src_->space_epoch;
    const auto inv = check_frame_desc(local, ctx);
    if (inv != frame_invalid_e::none) {
      last_invalid_ = inv;
      ++invalid_total;
      if (++invalid_streak_ >= kInvalidStreakLimit && !teardown_requested_) {
        teardown_requested_ = true;
        BOOST_LOG(warning) << "[VIPLE-VR-IPC] frame descriptors invalid x" << invalid_streak_ << " gen=" << src_->generation
                           << " last=" << frame_invalid_name(inv);
        bridge().request_teardown(src_->generation, VRIPC_BYE_PROTOCOL_ERROR);
      }
      return result_e::invalid;
    }
    invalid_streak_ = 0;
    last_invalid_ = frame_invalid_e::none;
    if (have_last_frame_) {
      last_skipped_ = local.frame_id - last_frame_id_ - 1;
      skipped_total += last_skipped_;
    }
    have_last_frame_ = true;
    last_frame_id_ = local.frame_id;
    last_fence_ = local.fence_value;

    // §B.8 第 4 條：echo 不在 server 最近 2 s 發布過的集合內 → 不算 echo
    int64_t pub = 0;
    if (!bridge().sample_recent(local.echo_sample_id, ctx.now_qpc, &pub)) {
      local.flags &= ~VRIPC_FRM_ECHO_MATCHED;
      local.flags |= VRIPC_FRM_POSE_FALLBACK;
      pub = 0;
    }
    last_echo_pub_qpc_ = (local.flags & VRIPC_FRM_ECHO_MATCHED) ? pub : 0;
    out = local;
    return result_e::ok;
  }

  void frame_reader_t::publish_consume(const vripc_consume_status_t &status) {
    if (!src_) {
      return;
    }
    const auto g = gen_of(*src_);
    if (g && g->shm) {
      std::memcpy((void *) &g->shm->consume, &status, sizeof status);
    }
  }

  fence_waiter_t::fence_waiter_t() {
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }

  fence_waiter_t::~fence_waiter_t() {
    if (event_) {
      CloseHandle(static_cast<HANDLE>(event_));
    }
  }

  fence_waiter_t::result_e fence_waiter_t::wait(ID3D11Fence *fence, uint64_t value, uint32_t timeout_ms, uint64_t *observed) {
    if (!fence || !event_) {
      return result_e::error;
    }
    const int64_t deadline = now_qpc() + ms_to_qpc(timeout_ms);
    for (;;) {
      const uint64_t v = fence->GetCompletedValue();
      if (observed) {
        *observed = v;
      }
      if (v == UINT64_MAX) {
        return result_e::lost;  // device removed（或對方把 fence Signal 到最大值）
      }
      if (registered_ && v >= registered_) {
        registered_ = 0;  // 之前的登記已到值（auto-reset event 可能留著訊號，醒來一律重讀）
      }
      if (v >= value) {
        return result_e::reached;
      }
      if (registered_ == 0 && SUCCEEDED(fence->SetEventOnCompletion(value, static_cast<HANDLE>(event_)))) {
        registered_ = value;  // 同一時間最多一個登記（sec-m10）
      }
      const int64_t rem = deadline - now_qpc();
      if (rem <= 0) {
        return result_e::timeout;
      }
      WaitForSingleObject(static_cast<HANDLE>(event_), ceil_ms(rem));
    }
  }

  // ── 對外 API ────────────────────────────────────────────────────────────
  bool start() {
    return bridge().start();
  }

  void stop() {
    bridge().stop();
  }

  status_t status() {
    return bridge().status();
  }

  bool set_session_config(const vripc_session_config_t &config) {
    return bridge().set_session_config(config);
  }

  void clear_session_config() {
    bridge().clear_session_config();
  }

  std::optional<luid_t> session_config_luid() {
    return bridge().session_config_luid();
  }

  void set_armed(bool armed) {
    bridge().set_armed(armed);
  }

  void request_steamvr_quit() {
    bridge().request_quit();
  }

  void set_pacing(const vripc_pacing_t &pacing) {
    bridge().set_pacing(pacing);
  }

  void set_pacing_ppm(int32_t ppm) {
    bridge().set_pacing_ppm(ppm);
  }

  void set_dev_mode(bool enabled) {
    bridge().set_dev_mode(enabled);
  }

  bool send_dev_set_v2p(uint32_t vsync_to_photons_us) {
    return bridge().send_v2p(vsync_to_photons_us);
  }

  void on_user_session_changed() {
    bridge().user_changed();
  }

  bool allow_selftest_peer(std::uintptr_t child_process_handle) {
    return bridge().allow_peer(child_process_handle);
  }

  bool allow_selftest_peer(void *child_process, const _FILETIME &created) {
    if (!child_process) {
      bridge().revoke_peer();
      return true;
    }
    return bridge().allow_peer(reinterpret_cast<std::uintptr_t>(child_process), &created);
  }

  void revoke_selftest_peer() {
    bridge().revoke_peer();
  }

  counters_t counters() {
    return bridge().counters();
  }

  selftest_objects_t duplicate_objects_for_selftest() {
    return bridge().duplicate_objects();
  }

  void set_expected_driver_host_image(const std::wstring &path) {
    bridge().set_host_image(path);
  }

  void request_teardown(uint64_t generation, uint32_t bye_reason) {
    bridge().request_teardown(generation, bye_reason);
  }

  void set_event_sink(event_cb cb) {
    bridge().set_event_sink(std::move(cb));
  }

  void set_haptic_sink(haptic_cb cb) {
    bridge().set_haptic_sink(std::move(cb));
  }

  void set_timing_sink(timing_cb cb) {
    bridge().set_timing_sink(std::move(cb));
  }

  bool tracking_wanted() {
    return bridge().tracking_wanted();
  }

  void publish_tracking(const tracked_sample_t &sample) {
    bridge().publish(sample);
  }

  std::shared_ptr<frame_source_t> frame_source() {
    return bridge().source();
  }

  pipe_sd_report_t check_pipe_security(bool set_security_roundtrip) {
    return bridge().check_pipe_security(set_security_roundtrip);
  }

  consume_report_t selftest_consume(std::chrono::milliseconds duration, const std::function<bool()> &should_stop) {
    consume_report_t rep;
    rep.supported = true;
    std::unique_lock busy(g_consume_mtx, std::try_to_lock);
    if (!busy.owns_lock()) {
      rep.error = "busy";
      return rep;
    }

    const int64_t deadline = now_qpc() + ms_to_qpc(duration.count());
    consumer_t cons;
    std::shared_ptr<frame_source_t> src;
    std::optional<frame_reader_t> reader;
    std::optional<fence_waiter_t> waiter;
    uint64_t last_consumed = 0;
    int64_t fence_timeout_since = 0;
    std::vector<double> visible_ms;
    std::vector<double> copy_ms;
    std::string last_error;
    uint64_t abandoned_gen = 0;  ///< 已要求拆除的 generation：等它真的拆掉，不重開

    auto signal_consumed = [&](uint64_t v) {
      // 只往上（latest-wins 跳過的 slot 一併放回），Signal 後一定 Flush（跨行程 CPU 觀察）
      v = std::max(v, last_consumed);
      cons.ctx4->Signal(cons.consumed_fence.get(), v);
      cons.ctx->Flush();
      last_consumed = v;
    };

    while (now_qpc() < deadline && !(should_stop && should_stop())) {
      auto cur = frame_source();
      if (!cur || !cur->alive() || cur->generation == abandoned_gen) {
        src.reset();
        Sleep(5);
        continue;
      }
      if (!src || cur->generation != src->generation) {
        src.reset();
        reader.reset();
        waiter.reset();
        if (auto err = cons.open(*cur); !err.empty()) {
          if (err != last_error) {
            BOOST_LOG(warning) << "[VIPLE-VR-IPC] selftest-consumer open gen=" << cur->generation << " failed: " << err;
            last_error = err;
          }
          rep.error = err;
          Sleep(100);
          continue;
        }
        src = cur;
        reader.emplace(src);
        waiter.emplace();
        last_consumed = cons.consumed_fence->GetCompletedValue();
        if (last_consumed == UINT64_MAX) {
          last_consumed = src->shared_fence_initial;
        }
        reader->set_fence_baseline(last_consumed);
        fence_timeout_since = 0;
        rep.generations_opened++;
        rep.ok = true;
        rep.error.clear();
        // 開完先「排空」：最新一筆的 slot 直接放回
        vripc_frame_desc_t d {};
        if (reader->read_latest(d) == frame_reader_t::result_e::ok) {
          signal_consumed(d.fence_value);
        }
      }

      if (WaitForSingleObject(static_cast<HANDLE>(src->evt_frm), 100) != WAIT_OBJECT_0) {
        rep.evt_timeouts++;
        continue;
      }
      rep.evt_wakes++;
      vripc_frame_desc_t d {};
      switch (reader->read_latest(d)) {
        case frame_reader_t::result_e::ok:
          break;
        case frame_reader_t::result_e::invalid:
          rep.frames_invalid++;
          continue;
        case frame_reader_t::result_e::torn:
          rep.frames_torn++;
          continue;
        case frame_reader_t::result_e::none:
          continue;
      }
      rep.frames_skipped += reader->last_skipped();
      rep.last_frame_id = d.frame_id;
      // §B.8 第 4 條的結果（read_latest 已依 server 最近 2 s 發布過的 sampleId 改寫旗標）
      if (d.flags & VRIPC_FRM_ECHO_MATCHED) {
        rep.frames_echo_matched++;
      } else if (d.flags & VRIPC_FRM_POSE_FALLBACK) {
        rep.frames_echo_fallback++;
      }

      const int64_t now = now_qpc();
      if (now - d.present_qpc > ms_to_qpc(100)) {
        rep.frames_stale++;
        signal_consumed(d.fence_value);
        continue;
      }

      uint64_t observed = 0;
      const auto fr = waiter->wait(cons.shared_fence.get(), d.fence_value, 2, &observed);
      if (fr == fence_waiter_t::result_e::lost) {
        rep.frames_fence_lost++;
        if (cons.dev->GetDeviceRemovedReason() == S_OK) {
          // 自己的 device 沒事 → driver 端問題：BYE(DEVICE_LOST)、新 generation
          request_teardown(src->generation, VRIPC_BYE_DEVICE_LOST);
          rep.teardown_requests++;
          abandoned_gen = src->generation;
        } else {
          cons.have_device = false;
        }
        src.reset();
        continue;
      }
      if (fr != fence_waiter_t::result_e::reached) {
        rep.frames_fence_timeout++;
        if (rep.frames_fence_timeout == 1) {
          // U7／E-4：記錄逾時當下 GetCompletedValue 的行為（對方停止 Signal 時應停在最後一次 Signal 的值）
          rep.fence_timeout_first_completed = observed;
          rep.fence_timeout_first_expected = d.fence_value;
        }
        signal_consumed(d.fence_value);  // 不複製，但仍放回 slot
        if (fence_timeout_since == 0) {
          fence_timeout_since = now;
        } else if (now - fence_timeout_since >= qpf()) {
          if (rep.fence_timeout_teardown_ms == 0) {
            rep.fence_timeout_teardown_ms = qpc_to_ms(now - fence_timeout_since);
          }
          request_teardown(src->generation, VRIPC_BYE_PROTOCOL_ERROR);  // 連續逾時累計 ≥ 1 s
          rep.teardown_requests++;
          fence_timeout_since = 0;
          abandoned_gen = src->generation;
          src.reset();
        }
        continue;
      }
      fence_timeout_since = 0;
      const int64_t visible = now_qpc();
      visible_ms.push_back(qpc_to_ms(visible - d.submit_qpc));

      // CPU 已確認 driver 的 GPU 寫完才下 CopyResource（不變式 8）
      const int64_t t0 = now_qpc();
      cons.ctx->CopyResource(cons.scratch.get(), cons.tex[d.tex_idx].get());
      cons.ctx->End(cons.query.get());
      signal_consumed(d.fence_value);
      const int64_t spin_deadline = t0 + ms_to_qpc(250);
      while (cons.ctx->GetData(cons.query.get(), nullptr, 0, 0) == S_FALSE && now_qpc() < spin_deadline) {
        std::this_thread::yield();
      }
      copy_ms.push_back(qpc_to_ms(now_qpc() - t0));
      rep.frames_copied++;

      vripc_consume_status_t cs {};
      cs.last_consumed_fence = last_consumed;
      cs.last_consumed_frame_id = d.frame_id;
      cs.frames_copied = rep.frames_copied;
      cs.frames_skipped_latest = rep.frames_skipped;
      cs.frames_fence_timeout = rep.frames_fence_timeout;
      cs.frames_invalid = rep.frames_invalid;
      cs.frames_device_lost = rep.frames_fence_lost;
      reader->publish_consume(cs);
    }

    percentiles(std::move(visible_ms), rep.submit_to_visible_p50_ms, rep.submit_to_visible_p95_ms, rep.submit_to_visible_max_ms);
    percentiles(std::move(copy_ms), rep.copy_p50_ms, rep.copy_p95_ms, rep.copy_max_ms);
    if (!rep.ok && rep.error.empty()) {
      rep.error = "no-frame-source";
    }
    return rep;
  }
}  // namespace vr::bridge
