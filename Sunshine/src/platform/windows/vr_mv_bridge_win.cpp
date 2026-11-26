/**
 * @file src/platform/windows/vr_mv_bridge_win.cpp
 * @brief VipleStream §VR-GAME-MV（M-MV1）：遊戲內運動向量外掛（UEVR / Unity MV Plugin）之 Named Pipe 與 SHM 橋接實作。
 *
 * 安全與架構不變式（比照 vr_bridge_win.cpp 與 vr_mv_ipc_abi.h 規則 A～D）：
 *   1. Pipe 建立時 SDDL 為 `O:SYG:SYD:P(A;;GA;;;SY)S:(ML;;NWNR;;;ME)`，加 `FILE_FLAG_FIRST_PIPE_INSTANCE` 與
 *      `PIPE_REJECT_REMOTE_CLIENTS`；主控台使用者登入後才加入該使用者 logon SID 的 `0x0012019B` ACE（不含建立新實例權限）。
 *   2. `verify()` 以 `GetNamedPipeClientProcessId` 前後兩次比對、確認建立時間早於連線且行程仍存活、同 session、
 *      連線與行程 token 皆為同一主控台使用者（Medium+、非 AppContainer），且 PID 必須等於 `vr::bridge::status().scene_pid`。
 *   3. `handshake()` 建立不具名 section（`VRMV_SECTION_SIZE`、SD 僅 SY），寫妥 `vrmv_server_block_t`，
 *      以最小權限 `FILE_MAP_READ | FILE_MAP_WRITE` `DuplicateHandle` 進遊戲行程；`WELCOME` 完整送達才移交所有權，失敗則收回。
 *   4. Server 絕不等待遊戲端任何同步物件；讀取 SHM 一律經 `vr::mv_pair` 的 seqlock 複製至本地緩衝並驗證。
 *   5. 所有 log 均以 `[VIPLE-VR-MV]` 為前綴，絕不印出 handle 值、完整 SID 或完整 GUID。
 */
#ifndef NOMINMAX
  #define NOMINMAX
#endif

// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// platform includes
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <wtsapi32.h>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/misc.h"
#include "src/platform/windows/vr_mv_bridge_win.h"
#include "src/vr/vr_bridge.h"
#include "src/vr/vr_mv_pair.h"

namespace vr::mv_bridge {

  namespace detail {
    std::atomic<bool> g_active_connected {false};
  }  // namespace detail

  namespace {
    /// 使用者 ACE：FILE_GENERIC_READ|FILE_WRITE_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES（不含 FILE_CREATE_PIPE_INSTANCE）
    constexpr DWORD kUserPipeAccess = 0x0012019Bu;
    constexpr DWORD kPipeBufferBytes = 4096;
    constexpr DWORD kHelloTimeoutMs = 2000;
    constexpr DWORD kWriteTimeoutMs = 100;
    constexpr DWORD kCloseWaitMs = 200;
    constexpr int64_t kHeartbeatMs = 100;
    constexpr int64_t kPipeRetryMs = 5000;
    constexpr int64_t kUserPollNoUserMs = 2000;
    constexpr int64_t kUserPollMs = 10000;

    /// BYE reason 常數（vrmv_bye_t.reason：0＝正常）
    constexpr uint32_t kByeNormal = 0u;
    constexpr uint32_t kByeSceneChanged = 1u;
    constexpr uint32_t kByeSessionEnd = 2u;

    int64_t now_qpc() noexcept {
      return platf::qpc_counter();
    }

    int64_t qpf() noexcept {
      const int64_t f = platf::qpc_frequency();
      return f > 0 ? f : 10'000'000;
    }

    int64_t ms_to_qpc(int64_t ms) noexcept {
      return ms * qpf() / 1000;
    }

    DWORD ceil_ms(int64_t ticks) noexcept {
      if (ticks <= 0) {
        return 0;
      }
      const int64_t ms = (ticks * 1000 + qpf() - 1) / qpf();
      return static_cast<DWORD>(std::clamp<int64_t>(ms, 1, 1000));
    }

    std::string version_text(uint32_t packed) {
      return std::format("{}.{}.{}", (packed >> 24) & 0xFFu, (packed >> 16) & 0xFFu, packed & 0xFFFFu);
    }

    const char *upscaler_name(uint32_t upscaler_kind) noexcept {
      switch (upscaler_kind) {
        case VRMV_UPSCALER_DLSS:
          return "dlss";
        default:
          return "unknown";
      }
    }

    // ── RAII ───────────────────────────────────────────────────────────────
    class handle_t {
    public:
      handle_t() = default;

      explicit handle_t(HANDLE h) noexcept:
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

      HANDLE get() const noexcept {
        return h_;
      }

      explicit operator bool() const noexcept {
        return h_ != nullptr && h_ != INVALID_HANDLE_VALUE;
      }

      void reset(HANDLE h = nullptr) noexcept {
        if (h_ != nullptr && h_ != INVALID_HANDLE_VALUE) {
          CloseHandle(h_);
        }
        h_ = h;
      }

    private:
      HANDLE h_ = nullptr;
    };

    class local_sd_t {
    public:
      ~local_sd_t() {
        if (p) {
          LocalFree(p);
        }
      }

      bool from_sddl(const wchar_t *sddl) noexcept {
        return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &p, nullptr) != FALSE;
      }

      PSECURITY_DESCRIPTOR p = nullptr;
    };

    // ── SID 與 Token 輔助 ──────────────────────────────────────────────────
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

    PSID as_psid(const sid_t &s) noexcept {
      return s.empty() ? nullptr : const_cast<PSID>(static_cast<const void *>(s.data()));
    }

    bool sid_equal(const sid_t &a, const sid_t &b) noexcept {
      return !a.empty() && !b.empty() && EqualSid(as_psid(a), as_psid(b)) != FALSE;
    }

    bool sid_equal(PSID a, const sid_t &b) noexcept {
      return a && !b.empty() && IsValidSid(a) && EqualSid(a, as_psid(b)) != FALSE;
    }

    uint32_t sid_rid(PSID s) noexcept {
      if (!s || !IsValidSid(s)) {
        return 0;
      }
      const UCHAR n = *GetSidSubAuthorityCount(s);
      return n ? static_cast<uint32_t>(*GetSidSubAuthority(s, n - 1)) : 0;
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
      bool ok = false;
      sid_t user;
      sid_t logon;
      uint32_t integrity = 0;
      bool app_container = true;
    };

    token_ident_t query_token(HANDLE tok) {
      token_ident_t r;
      if (auto u = token_info(tok, TokenUser); u.size() >= sizeof(TOKEN_USER)) {
        r.user = copy_sid(reinterpret_cast<TOKEN_USER *>(u.data())->User.Sid);
      }
      if (auto l = token_info(tok, TokenLogonSid); l.size() >= sizeof(TOKEN_GROUPS)) {
        auto *g = reinterpret_cast<TOKEN_GROUPS *>(l.data());
        if (g->GroupCount >= 1) {
          r.logon = copy_sid(g->Groups[0].Sid);
        }
      }
      if (r.logon.empty()) {
        if (auto gr = token_info(tok, TokenGroups); gr.size() >= sizeof(TOKEN_GROUPS)) {
          auto *g = reinterpret_cast<TOKEN_GROUPS *>(gr.data());
          const size_t max_groups = gr.size() >= offsetof(TOKEN_GROUPS, Groups) ?
                                      (gr.size() - offsetof(TOKEN_GROUPS, Groups)) / sizeof(SID_AND_ATTRIBUTES) :
                                      0;
          const DWORD count = std::min<DWORD>(g->GroupCount, static_cast<DWORD>(max_groups));
          for (DWORD i = 0; i < count; ++i) {
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

    class pipe_impersonation_t {
    public:
      explicit pipe_impersonation_t(HANDLE pipe) noexcept {
        ok_ = ImpersonateNamedPipeClient(pipe) != FALSE;
        if (!ok_) {
          err_ = GetLastError();
        }
      }

      ~pipe_impersonation_t() {
        if (ok_ && !RevertToSelf()) {
          BOOST_LOG(fatal) << "[VIPLE-VR-MV] RevertToSelf failed after pipe impersonation: " << GetLastError();
          std::terminate();
        }
      }

      pipe_impersonation_t(const pipe_impersonation_t &) = delete;
      pipe_impersonation_t &operator=(const pipe_impersonation_t &) = delete;

      bool ok() const noexcept {
        return ok_;
      }

      DWORD error() const noexcept {
        return err_;
      }

    private:
      bool ok_ = false;
      DWORD err_ = 0;
    };

    void fill_hdr(vrmv_msg_hdr_t &h, uint16_t type, uint32_t size, uint32_t seq) noexcept {
      h.magic = VRMV_MAGIC_PIPE;
      h.type = type;
      h.flags = 0;
      h.size = size;
      h.seq = seq;
    }

    // ── Generation 與連線結構 ───────────────────────────────────────────────
    struct gen_t {
      uint64_t generation = 0;
      handle_t section;
      vrmv_shm_t *shm = nullptr;
      std::atomic<bool> alive {true};

      ~gen_t() {
        if (shm) {
          UnmapViewOfFile(shm);
          shm = nullptr;
        }
      }
    };

    struct conn_t {
      handle_t proc;
      DWORD pid = 0;
      FILETIME created {};
      uint32_t plugin_version_packed = 0;
      uint32_t upscaler_kind = 0;
      uint32_t out_seq = 0;
      uint32_t in_seq = 0;
      std::shared_ptr<gen_t> gen;
      int64_t welcome_qpc = 0;
      int64_t next_heartbeat = 0;
    };

    enum class io_e {
      ok,
      timeout,
      too_big,
      failed,
      stopped,
    };

    struct verify_result_t {
      bool ok = false;
      uint32_t reject = 0;
      uint32_t scene_pid = 0;
      std::string reason;
      conn_t conn;
    };

    // ── Bridge 主類別 ─────────────────────────────────────────────────────
    class mv_bridge_t {
    public:
      bool start();
      void stop();
      bool is_running() noexcept;
      std::shared_ptr<gen_t> current_gen() noexcept;

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
      void handshake(conn_t &&cin);
      void handle_message(const uint8_t *buf, DWORD n);
      void periodic(int64_t now);
      void teardown(const std::string &reason, bool send_bye, uint32_t bye_reason, bool wait_close = true);
      void close_wait(DWORD ms);

      io_e issue_read(uint8_t *buf, DWORD cap);
      io_e complete_read(DWORD &n);
      void cancel_read();
      io_e read_sync(uint8_t *buf, DWORD cap, DWORD timeout_ms, DWORD &n);
      bool write_msg(const void *msg, DWORD size);
      void start_read();
      DWORD compute_timeout(int64_t now, int64_t next_pipe_try, int64_t next_user_poll) const noexcept;

      std::mutex life_mtx_;
      std::thread thread_;
      handle_t ev_stop_;
      handle_t ev_read_;
      handle_t ev_write_;
      std::atomic<bool> running_ {false};

      std::mutex pipe_mtx_;
      handle_t pipe_;
      OVERLAPPED ov_read_ {};
      OVERLAPPED ov_write_ {};
      bool listening_ = false;
      bool read_pending_ = false;
      DWORD last_io_error_ = 0;
      std::array<uint8_t, VRMV_PIPE_MAX_MSG> rbuf_ {};

      DWORD self_session_ = 0xFFFFFFFFu;
      std::optional<user_t> user_;
      std::optional<conn_t> conn_;
      bool in_teardown_ = false;
      int64_t backoff_until_ = 0;
      uint64_t generation_counter_ = 0;
      std::string pipe_reason_logged_;

      std::mutex gen_mtx_;
      std::shared_ptr<gen_t> cur_gen_;
    };

    mv_bridge_t &instance() {
      static mv_bridge_t *b = new mv_bridge_t();
      return *b;
    }

    bool mv_bridge_t::start() {
      if (config::vr.pcvr == config::vr_t::pcvr_e::disabled ||
          config::vr.game_mv == config::vr_t::game_mv_e::disabled) {
        return false;
      }
      std::lock_guard lk(life_mtx_);
      if (thread_.joinable()) {
        return true;
      }
      if (!ev_stop_) {
        ev_stop_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ev_read_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ev_write_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
      }
      if (!ev_stop_ || !ev_read_ || !ev_write_) {
        BOOST_LOG(error) << "[VIPLE-VR-MV] disabled reason=event-create-failed err=" << GetLastError();
        ev_stop_.reset();
        ev_read_.reset();
        ev_write_.reset();
        return false;
      }
      ResetEvent(ev_stop_.get());
      running_.store(true, std::memory_order_release);
      thread_ = std::thread(&mv_bridge_t::thread_main, this);
      return true;
    }

    void mv_bridge_t::stop() {
      std::lock_guard lk(life_mtx_);
      if (!thread_.joinable()) {
        return;
      }
      SetEvent(ev_stop_.get());
      thread_.join();
      running_.store(false, std::memory_order_release);
    }

    bool mv_bridge_t::is_running() noexcept {
      return running_.load(std::memory_order_acquire);
    }

    std::shared_ptr<gen_t> mv_bridge_t::current_gen() noexcept {
      std::lock_guard lk(gen_mtx_);
      return cur_gen_;
    }

    // ── Pipe IO ────────────────────────────────────────────────────────────
    io_e mv_bridge_t::issue_read(uint8_t *buf, DWORD cap) {
      ov_read_ = {};
      ov_read_.hEvent = ev_read_.get();
      ResetEvent(ev_read_.get());
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

    io_e mv_bridge_t::complete_read(DWORD &n) {
      read_pending_ = false;
      n = 0;
      if (GetOverlappedResult(pipe_.get(), &ov_read_, &n, FALSE)) {
        return io_e::ok;
      }
      const DWORD e = GetLastError();
      last_io_error_ = e;
      return e == ERROR_MORE_DATA ? io_e::too_big : io_e::failed;
    }

    void mv_bridge_t::cancel_read() {
      if (!read_pending_) {
        return;
      }
      CancelIoEx(pipe_.get(), &ov_read_);
      DWORD n = 0;
      GetOverlappedResult(pipe_.get(), &ov_read_, &n, TRUE);
      read_pending_ = false;
    }

    io_e mv_bridge_t::read_sync(uint8_t *buf, DWORD cap, DWORD timeout_ms, DWORD &n) {
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

    bool mv_bridge_t::write_msg(const void *msg, DWORD size) {
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

    void mv_bridge_t::start_read() {
      if (!conn_ || read_pending_) {
        return;
      }
      const auto r = issue_read(rbuf_.data(), static_cast<DWORD>(rbuf_.size()));
      if (r == io_e::too_big) {
        teardown("message-too-big", true, kByeNormal);
      } else if (r != io_e::ok) {
        teardown(std::format("pipe-closed err={}", last_io_error_), false, 0);
      }
    }

    void mv_bridge_t::close_wait(DWORD ms) {
      const int64_t deadline = now_qpc() + ms_to_qpc(ms);
      for (;;) {
        if (!read_pending_ && issue_read(rbuf_.data(), static_cast<DWORD>(rbuf_.size())) != io_e::ok) {
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
          break;
        }
      }
      cancel_read();
    }

    // ── Pipe 建立與 DACL ──────────────────────────────────────────────────
    bool mv_bridge_t::create_pipe() {
      DWORD self = 0xFFFFFFFFu;
      if (!ProcessIdToSessionId(GetCurrentProcessId(), &self)) {
        self = 0xFFFFFFFFu;
      }
      const DWORD console = WTSGetActiveConsoleSessionId();
      self_session_ = self;

      std::string reason;
      if (self == 0xFFFFFFFFu || self != console) {
        reason = "session-mismatch";
      } else {
        local_sd_t sd;
        if (!sd.from_sddl(L"O:SYG:SYD:P(A;;GA;;;SY)S:(ML;;NWNR;;;ME)")) {
          reason = std::format("sddl-error-{}", GetLastError());
        } else {
          SECURITY_ATTRIBUTES sa {sizeof(SECURITY_ATTRIBUTES), sd.p, FALSE};
          const std::wstring name = std::wstring(VRMV_PIPE_NAME_PREFIX) + std::to_wstring(self);
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
              reason = "not-system";
            } else if (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY) {
              reason = "squatted";
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
        if (reason != pipe_reason_logged_) {
          pipe_reason_logged_ = reason;
          BOOST_LOG(warning) << "[VIPLE-VR-MV] pipe unavailable reason=" << reason
                             << " self=" << static_cast<int64_t>(static_cast<int32_t>(self))
                             << " console=" << static_cast<int64_t>(static_cast<int32_t>(console));
        }
        return false;
      }

      pipe_reason_logged_ = "ok";
      BOOST_LOG(info) << "[VIPLE-VR-MV] pipe ready session=" << self << " dacl=system-only";
      return true;
    }

    void mv_bridge_t::refresh_user() {
      const DWORD console = WTSGetActiveConsoleSessionId();
      std::optional<user_t> u;
      if (console == self_session_) {
        u = query_session_user(self_session_);
      }
      const bool same = (u && user_ && sid_equal(u->logon, user_->logon)) || (!u && !user_);
      if (same) {
        if (user_ && !pipe_has_user_ace(user_->logon)) {
          const DWORD err = apply_user_dacl(user_);
          if (err == ERROR_SUCCESS) {
            BOOST_LOG(warning) << "[VIPLE-VR-MV] user-ace reapplied sid=*" << sid_rid(as_psid(user_->user));
          } else {
            BOOST_LOG(error) << "[VIPLE-VR-MV] user-ace reapply failed err=" << err;
          }
        }
        return;
      }

      if (conn_) {
        teardown("user-changed", true, kByeNormal);
      }
      stop_listen();

      const DWORD err = apply_user_dacl(u);
      if (err != ERROR_SUCCESS) {
        BOOST_LOG(error) << "[VIPLE-VR-MV] user-ace update failed err=" << err;
        user_.reset();
        return;
      }

      user_ = std::move(u);
    }

    DWORD mv_bridge_t::apply_user_dacl(const std::optional<user_t> &u) {
      const auto sys = well_known_sid(WinLocalSystemSid);
      const DWORD acl_size = static_cast<DWORD>((sizeof(ACL) + 2 * sizeof(ACCESS_ALLOWED_ACE) + sys.size() + (u ? u->logon.size() : 0) + 16 + 3) & ~size_t {3});
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
      if (!pipe_) {
        return ERROR_INVALID_HANDLE;
      }
      return SetSecurityInfo(pipe_.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    }

    bool mv_bridge_t::pipe_has_user_ace(const sid_t &logon) {
      std::lock_guard lk(pipe_mtx_);
      if (!pipe_) {
        return true;
      }
      PACL dacl = nullptr;
      PSECURITY_DESCRIPTOR sd = nullptr;
      if (GetSecurityInfo(pipe_.get(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return true;
      }
      bool found = false;
      for (DWORD i = 0; dacl && i < dacl->AceCount && !found; ++i) {
        void *p = nullptr;
        if (!GetAce(dacl, i, &p)) {
          continue;
        }
        const auto *h = static_cast<const ACE_HEADER *>(p);
        if (h->AceType != ACCESS_ALLOWED_ACE_TYPE || h->AceSize < sizeof(ACCESS_ALLOWED_ACE)) {
          continue;
        }
        auto *a = static_cast<ACCESS_ALLOWED_ACE *>(p);
        found = a->Mask == kUserPipeAccess && sid_equal(reinterpret_cast<PSID>(&a->SidStart), logon);
      }
      LocalFree(sd);
      return found;
    }

    void mv_bridge_t::start_listen() {
      ov_read_ = {};
      ov_read_.hEvent = ev_read_.get();
      ResetEvent(ev_read_.get());
      if (ConnectNamedPipe(pipe_.get(), &ov_read_)) {
        on_connected();
        return;
      }
      const DWORD e = GetLastError();
      if (e == ERROR_IO_PENDING) {
        listening_ = true;
        return;
      }
      if (e == ERROR_PIPE_CONNECTED) {
        on_connected();
        return;
      }
      DisconnectNamedPipe(pipe_.get());
      backoff_until_ = now_qpc() + ms_to_qpc(e == ERROR_NO_DATA ? 100 : 1000);
      if (e != ERROR_NO_DATA) {
        BOOST_LOG(warning) << "[VIPLE-VR-MV] ConnectNamedPipe failed err=" << e;
      }
    }

    void mv_bridge_t::stop_listen() {
      if (!listening_) {
        return;
      }
      CancelIoEx(pipe_.get(), &ov_read_);
      DWORD n = 0;
      GetOverlappedResult(pipe_.get(), &ov_read_, &n, TRUE);
      listening_ = false;
      DisconnectNamedPipe(pipe_.get());
    }

    // ── 連線、驗證、握手 ──────────────────────────────────────────────────
    void mv_bridge_t::on_read_event() {
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
        teardown("message-too-big", true, kByeNormal);
      } else {
        teardown(std::format("pipe-closed err={}", last_io_error_), false, 0);
      }
    }

    void mv_bridge_t::on_connected() {
      FILETIME connected_at {};
      GetSystemTimeAsFileTime(&connected_at);
      auto r = verify(connected_at);
      if (!r.ok) {
        reject_and_disconnect(r);
        return;
      }
      handshake(std::move(r.conn));
    }

    verify_result_t mv_bridge_t::verify(const FILETIME &connected_at) {
      verify_result_t r;
      auto &c = r.conn;

      // 1. Client PID 前後兩次比對、建立時間早於連線、行程仍存活
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
      FILETIME ex {}, kt {}, ut {};
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

      // 2. 與 server 同一個 Windows Session
      ULONG csess = 0xFFFFFFFFu;
      if (!GetNamedPipeClientSessionId(pipe_.get(), &csess) || csess != self_session_) {
        r.reason = std::format("session client={} self={}", static_cast<int64_t>(static_cast<int32_t>(csess)), self_session_);
        return r;
      }

      // 3. 讀取第一則訊息並驗證凍結的 24 B 標頭
      std::array<uint8_t, VRMV_PIPE_MAX_MSG> buf {};
      DWORD n = 0;
      const auto rr = read_sync(buf.data(), static_cast<DWORD>(buf.size()), kHelloTimeoutMs, n);
      if (rr == io_e::timeout) {
        r.reject = VRMV_REJ_BAD_MESSAGE;
        r.reason = "hello-timeout";
        return r;
      }
      if (rr != io_e::ok) {
        r.reason = rr == io_e::too_big ? "not-protocol size>max" : (rr == io_e::stopped ? "stopping" : std::format("hello-read err={}", last_io_error_));
        return r;
      }
      vrmv_msg_hdr_t hdr {};
      if (n >= sizeof(hdr)) {
        std::memcpy(&hdr, buf.data(), sizeof(hdr));
      }
      if (n < 24 || hdr.size != n || hdr.magic != VRMV_MAGIC_PIPE || hdr.type != VRMV_MSG_HELLO || hdr.flags != 0) {
        r.reason = std::format("not-protocol bytes={}", n);
        return r;
      }
      c.in_seq = hdr.seq;

      // 4. Token 驗證：連線 token 與行程 token 皆須為同一主控台使用者、Medium+ 且非 AppContainer
      if (!user_) {
        r.reject = VRMV_REJ_IDENTITY;
        r.reason = "no-console-user";
        return r;
      }
      handle_t conn_tok;
      {
        pipe_impersonation_t imp(pipe_.get());
        if (!imp.ok()) {
          r.reject = VRMV_REJ_IDENTITY;
          r.reason = std::format("impersonate err={}", imp.error());
          return r;
        }
        HANDLE t = nullptr;
        if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &t)) {
          conn_tok.reset(t);
        }
      }
      handle_t proc_tok;
      {
        HANDLE t = nullptr;
        if (OpenProcessToken(hp, TOKEN_QUERY, &t)) {
          proc_tok.reset(t);
        }
      }
      if (!conn_tok || !proc_tok) {
        r.reject = VRMV_REJ_IDENTITY;
        r.reason = std::format("token-open conn={} proc={}", static_cast<bool>(conn_tok), static_cast<bool>(proc_tok));
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
          r.reject = VRMV_REJ_IDENTITY;
          r.reason = std::format("token-{} {} sid=*{}", which, fail, id.user.empty() ? 0 : sid_rid(as_psid(id.user)));
          return r;
        }
      }

      // 5. 檢查 client PID 必須等於目前的 scene_pid（scene_pid 為 0 或不符 → REJECT IDENTITY）
      const uint32_t cur_scene_pid = vr::bridge::status().scene_pid;
      r.scene_pid = cur_scene_pid;
      if (cur_scene_pid == 0 || pid != cur_scene_pid) {
        r.reject = VRMV_REJ_IDENTITY;
        r.reason = std::format("scene-pid-mismatch client={} scene={}", pid, cur_scene_pid);
        return r;
      }

      // 6. HELLO 驗證：先比對 abi_version，再驗訊息大小
      uint32_t abi = 0;
      if (n >= offsetof(vrmv_hello_t, abi_version) + sizeof(abi)) {
        std::memcpy(&abi, buf.data() + offsetof(vrmv_hello_t, abi_version), sizeof(abi));
      }
      if (abi != VRMV_ABI_VERSION) {
        r.reject = VRMV_REJ_ABI_MISMATCH;
        r.reason = std::format("abi-mismatch peer={} server={}", abi, VRMV_ABI_VERSION);
        return r;
      }
      if (n != sizeof(vrmv_hello_t)) {
        r.reject = VRMV_REJ_BAD_MESSAGE;
        r.reason = std::format("hello-size bytes={}", n);
        return r;
      }
      vrmv_hello_t hello {};
      std::memcpy(&hello, buf.data(), sizeof(hello));

      c.plugin_version_packed = hello.plugin_version_packed;
      c.upscaler_kind = hello.upscaler_kind;
      r.ok = true;
      return r;
    }

    void mv_bridge_t::reject_and_disconnect(verify_result_t &r) {
      if (r.reject != 0) {
        vrmv_reject_t m {};
        fill_hdr(m.hdr, VRMV_MSG_REJECT, sizeof(m), 1);
        m.reason = r.reject;
        m.reserved = 0;
        write_msg(&m, sizeof(m));
        close_wait(kCloseWaitMs);
      }
      DisconnectNamedPipe(pipe_.get());
      backoff_until_ = now_qpc() + ms_to_qpc(500);
      BOOST_LOG(warning) << "[VIPLE-VR-MV] rejected reason=\"" << r.reason << "\" rej=" << r.reject
                         << " pid=" << r.conn.pid << " scenePid=" << r.scene_pid;
    }

    void mv_bridge_t::handshake(conn_t &&cin) {
      conn_t c = std::move(cin);
      auto gen = std::make_shared<gen_t>();
      gen->generation = ++generation_counter_;

      // 建立不具名 section（VRMV_SECTION_SIZE、SD 只有 SY）
      std::string fail;
      local_sd_t sd;
      SECURITY_ATTRIBUTES sa {sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
      if (sd.from_sddl(L"O:SYG:SYD:P(A;;GA;;;SY)")) {
        sa.lpSecurityDescriptor = sd.p;
      }
      SetLastError(0);
      gen->section.reset(CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, VRMV_SECTION_SIZE, nullptr));
      if (!gen->section || GetLastError() == ERROR_ALREADY_EXISTS) {
        fail = std::format("section err={}", GetLastError());
      } else {
        gen->shm = static_cast<vrmv_shm_t *>(MapViewOfFile(gen->section.get(), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, VRMV_SECTION_SIZE));
        if (!gen->shm) {
          fail = std::format("map err={}", GetLastError());
        }
      }
      if (!fail.empty()) {
        BOOST_LOG(error) << "[VIPLE-VR-MV] handshake failed stage=create " << fail;
        verify_result_t r;
        r.reject = VRMV_REJ_BUSY;
        r.reason = "server-resource " + fail;
        r.scene_pid = c.pid;
        r.conn = std::move(c);
        reject_and_disconnect(r);
        return;
      }

      // 初始化 SHM Header 與 Server Block
      const int64_t now = now_qpc();
      auto *shm = gen->shm;
      std::memset(shm, 0, VRMV_HEADER_BYTES);
      shm->server.magic = VRMV_MAGIC_SHM;
      shm->server.abi_version = VRMV_ABI_VERSION;
      shm->server.section_size = VRMV_SECTION_SIZE;
      shm->server.generation = gen->generation;
      shm->server.qpc_frequency = qpf();
      shm->server.server_pid = GetCurrentProcessId();
      shm->server.last_consumed_mv_frame_id = 0;
      vrmv_store_release_u64(&shm->server.server_heartbeat_qpc, static_cast<uint64_t>(now));

      // 以 FILE_MAP_READ | FILE_MAP_WRITE 最小權限複製 section handle 進遊戲行程
      HANDLE remote = nullptr;
      if (!DuplicateHandle(GetCurrentProcess(), gen->section.get(), c.proc.get(), &remote, FILE_MAP_READ | FILE_MAP_WRITE, FALSE, 0)) {
        const DWORD dup_err = GetLastError();
        BOOST_LOG(error) << "[VIPLE-VR-MV] handshake failed stage=duplicate err=" << dup_err;
        verify_result_t r;
        r.reject = VRMV_REJ_BUSY;
        r.reason = std::format("duplicate err={}", dup_err);
        r.scene_pid = c.pid;
        r.conn = std::move(c);
        reject_and_disconnect(r);
        return;
      }
      const uint64_t r_shm = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(remote));

      vrmv_welcome_t w {};
      fill_hdr(w.hdr, VRMV_MSG_WELCOME, sizeof(w), ++c.out_seq);
      w.abi_version = VRMV_ABI_VERSION;
      w.server_pid = GetCurrentProcessId();
      w.generation = gen->generation;
      w.section_size = VRMV_SECTION_SIZE;
      w.qpc_frequency = qpf();
      w.dup_section_handle = r_shm;
      if (!write_msg(&w, sizeof(w))) {
        // 送出失敗才從遊戲行程收回 handle；成功送達則移交所有權，server 絕不再關
        DuplicateHandle(c.proc.get(), reinterpret_cast<HANDLE>(static_cast<uintptr_t>(r_shm)), nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
        BOOST_LOG(warning) << "[VIPLE-VR-MV] handshake failed stage=welcome-write err=" << last_io_error_;
        close_wait(kCloseWaitMs);
        DisconnectNamedPipe(pipe_.get());
        backoff_until_ = now_qpc() + ms_to_qpc(1000);
        return;
      }

      c.gen = gen;
      c.welcome_qpc = now_qpc();
      c.next_heartbeat = c.welcome_qpc + ms_to_qpc(kHeartbeatMs);
      const DWORD cpid = c.pid;
      const uint32_t pver = c.plugin_version_packed;
      const uint32_t upk = c.upscaler_kind;
      conn_ = std::move(c);
      {
        std::lock_guard lk(gen_mtx_);
        cur_gen_ = gen;
      }
      detail::g_active_connected.store(true, std::memory_order_release);

      BOOST_LOG(info) << "[VIPLE-VR-MV] connected gen=" << gen->generation
                      << " pid=" << cpid
                      << " plugin=" << version_text(pver)
                      << " upscaler=" << upscaler_name(upk)
                      << " abi=" << VRMV_ABI_VERSION;
      start_read();
    }

    void mv_bridge_t::handle_message(const uint8_t *buf, DWORD n) {
      if (!conn_) {
        return;
      }
      auto &c = *conn_;
      vrmv_msg_hdr_t hdr {};
      if (n < sizeof(hdr)) {
        teardown(std::format("short-message bytes={}", n), true, kByeNormal);
        return;
      }
      std::memcpy(&hdr, buf, sizeof(hdr));
      if (hdr.magic != VRMV_MAGIC_PIPE || hdr.size != n || hdr.flags != 0 || hdr.seq <= c.in_seq) {
        teardown(std::format("bad-header bytes={} seq={}", n, hdr.seq), true, kByeNormal);
        return;
      }
      c.in_seq = hdr.seq;

      if (hdr.type == VRMV_MSG_BYE) {
        if (n != sizeof(vrmv_bye_t)) {
          teardown("bye-size", true, kByeNormal);
          return;
        }
        vrmv_bye_t b {};
        std::memcpy(&b, buf, sizeof(b));
        teardown(std::format("plugin-bye reason={}", b.reason), false, 0);
        return;
      }
      teardown(std::format("unexpected-msg type={}", hdr.type), true, kByeNormal);
    }

    void mv_bridge_t::periodic(int64_t now) {
      if (!conn_) {
        return;
      }
      auto &c = *conn_;
      if (now < c.next_heartbeat) {
        return;
      }
      c.next_heartbeat = now + ms_to_qpc(kHeartbeatMs);

      if (c.gen && c.gen->shm) {
        vrmv_store_release_u64(&c.gen->shm->server.server_heartbeat_qpc, static_cast<uint64_t>(now));
      }

      // 檢查遊戲行程是否已結束
      if (WaitForSingleObject(c.proc.get(), 0) != WAIT_TIMEOUT) {
        teardown("process-exited", false, 0);
        return;
      }

      // 檢查目前的 scene_pid 是否改變
      const uint32_t cur_scene_pid = vr::bridge::status().scene_pid;
      if (cur_scene_pid != c.pid) {
        teardown(std::format("scene-pid-changed old={} new={}", c.pid, cur_scene_pid), true, kByeSceneChanged);
        return;
      }
    }

    void mv_bridge_t::teardown(const std::string &reason, bool send_bye, uint32_t bye_reason, bool wait_close) {
      if (!conn_ || in_teardown_) {
        return;
      }
      in_teardown_ = true;
      detail::g_active_connected.store(false, std::memory_order_release);

      auto &c = *conn_;
      const uint64_t gen_id = c.gen ? c.gen->generation : 0;
      const DWORD pid = c.pid;

      // 使 generation 作廢：清掉 cur_gen_、alive 設 false、SHM server block 的 generation 與 heartbeat 歸零
      if (c.gen) {
        c.gen->alive.store(false, std::memory_order_release);
        if (c.gen->shm) {
          vrmv_store_release_u64(&c.gen->shm->server.generation, 0);
          vrmv_store_release_u64(&c.gen->shm->server.server_heartbeat_qpc, 0);
        }
      }
      {
        std::lock_guard lk(gen_mtx_);
        cur_gen_.reset();
      }

      if (send_bye) {
        vrmv_bye_t b {};
        fill_hdr(b.hdr, VRMV_MSG_BYE, sizeof(b), ++c.out_seq);
        b.reason = bye_reason;
        b.reserved = 0;
        if (!write_msg(&b, sizeof(b))) {
          wait_close = false;
        }
      }
      if (wait_close) {
        close_wait(kCloseWaitMs);
      } else {
        cancel_read();
      }
      DisconnectNamedPipe(pipe_.get());

      BOOST_LOG(info) << "[VIPLE-VR-MV] disconnected gen=" << gen_id << " pid=" << pid << " reason=\"" << reason << "\""
                      << (send_bye ? std::format(" bye={}", bye_reason) : std::string {});

      conn_.reset();
      in_teardown_ = false;
    }

    DWORD mv_bridge_t::compute_timeout(int64_t now, int64_t next_pipe_try, int64_t next_user_poll) const noexcept {
      if (conn_) {
        return std::clamp<DWORD>(ceil_ms(conn_->next_heartbeat - now), 1, static_cast<DWORD>(kHeartbeatMs));
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

    void mv_bridge_t::thread_main() {
      platf::set_thread_name("vr_mv_bridge");
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
        if (pipe_ && now_qpc() >= next_user_poll) {
          refresh_user();
          next_user_poll = now_qpc() + ms_to_qpc(user_ ? kUserPollMs : kUserPollNoUserMs);
        }
        now = now_qpc();
        if (pipe_ && !conn_ && !listening_ && user_ && now >= backoff_until_) {
          start_listen();
        }
        if (conn_) {
          periodic(now_qpc());
        }

        HANDLE hs[2];
        DWORD n = 0;
        hs[n++] = ev_stop_.get();
        DWORD read_idx = MAXDWORD;
        if (listening_ || (conn_ && read_pending_)) {
          read_idx = n;
          hs[n++] = ev_read_.get();
        }
        const DWORD w = WaitForMultipleObjects(n, hs, FALSE, compute_timeout(now_qpc(), next_pipe_try, next_user_poll));
        if (w == WAIT_FAILED) {
          BOOST_LOG(error) << "[VIPLE-VR-MV] WaitForMultipleObjects failed err=" << GetLastError();
          Sleep(10);
          continue;
        }
        if (w == WAIT_OBJECT_0) {
          break;
        }
        if (read_idx != MAXDWORD && w == WAIT_OBJECT_0 + read_idx) {
          on_read_event();
        }
      }
      shutdown_all();
    }

    void mv_bridge_t::shutdown_all() {
      if (conn_) {
        teardown("session-end", true, kByeSessionEnd);
      }
      stop_listen();
      {
        std::lock_guard lk(pipe_mtx_);
        pipe_.reset();
      }
      user_.reset();
      pipe_reason_logged_.clear();
      detail::g_active_connected.store(false, std::memory_order_release);
      {
        std::lock_guard lk(gen_mtx_);
        cur_gen_.reset();
      }
    }
  }  // namespace

  // ── 對外公開介面 ──────────────────────────────────────────────────────────
  bool start() {
    return instance().start();
  }

  void stop() {
    instance().stop();
  }

  bool is_running() noexcept {
    return instance().is_running();
  }

  bool peek_write_index(uint64_t &out_write_index, int64_t *out_writer_heartbeat_qpc) noexcept {
    if (!is_active()) {
      return false;
    }
    const auto g = instance().current_gen();
    if (!g || !g->shm || !g->alive.load(std::memory_order_acquire)) {
      return false;
    }
    out_write_index = vrmv_load_acquire_u64(&g->shm->write_index);
    if (out_writer_heartbeat_qpc) {
      *out_writer_heartbeat_qpc = static_cast<int64_t>(
        vrmv_load_acquire_u64(&g->shm->writer.writer_heartbeat_qpc)
      );
    }
    return true;
  }

  bool snapshot_latest(
    std::array<vrmv_desc_t, VRMV_SLOTS> &out_descs,
    size_t &out_count,
    uint64_t *out_write_index,
    int64_t *out_writer_heartbeat_qpc,
    vrmv_writer_block_t *out_writer_block
  ) noexcept {
    out_count = 0;
    if (!is_active()) {
      return false;
    }
    const auto g = instance().current_gen();
    if (!g || !g->shm || !g->alive.load(std::memory_order_acquire)) {
      return false;
    }
    if (out_write_index) {
      *out_write_index = vrmv_load_acquire_u64(&g->shm->write_index);
    }
    if (out_writer_heartbeat_qpc) {
      *out_writer_heartbeat_qpc = static_cast<int64_t>(
        vrmv_load_acquire_u64(&g->shm->writer.writer_heartbeat_qpc)
      );
    }
    out_count = vr::mv_pair::shm_snapshot_descs(g->shm, g->generation, out_descs.data(), out_descs.size());
    if (out_writer_block) {
      std::memcpy(out_writer_block, static_cast<const void *>(&g->shm->writer), sizeof(vrmv_writer_block_t));
    }
    return true;
  }

  bool copy_payload(const vrmv_desc_t &desc, void *dst, size_t dst_capacity) noexcept {
    if (!is_active()) {
      return false;
    }
    const auto g = instance().current_gen();
    if (!g || !g->shm || !g->alive.load(std::memory_order_acquire)) {
      return false;
    }
    if (!vr::mv_pair::shm_copy_payload(g->shm, desc, dst, dst_capacity)) {
      return false;
    }
    vrmv_store_release_u64(&g->shm->server.last_consumed_mv_frame_id, desc.mv_frame_id);
    return true;
  }

  bool copy_payload(uint64_t mv_frame_id, void *dst, size_t dst_capacity, vrmv_desc_t *out_desc) noexcept {
    if (mv_frame_id == 0 || !is_active()) {
      return false;
    }
    const auto g = instance().current_gen();
    if (!g || !g->shm || !g->alive.load(std::memory_order_acquire)) {
      return false;
    }
    vrmv_desc_t d {};
    const uint32_t slot = static_cast<uint32_t>(mv_frame_id % VRMV_SLOTS);
    if (!vr::mv_pair::shm_read_slot_with_payload(g->shm, slot, g->generation, d, dst, dst_capacity)) {
      return false;
    }
    if (d.mv_frame_id != mv_frame_id) {
      return false;
    }
    if (out_desc) {
      *out_desc = d;
    }
    vrmv_store_release_u64(&g->shm->server.last_consumed_mv_frame_id, d.mv_frame_id);
    return true;
  }

  bool read_writer_block(vrmv_writer_block_t &out_writer, uint64_t *out_generation) noexcept {
    if (!is_active()) {
      return false;
    }
    const auto g = instance().current_gen();
    if (!g || !g->shm || !g->alive.load(std::memory_order_acquire)) {
      return false;
    }
    std::memcpy(&out_writer, static_cast<const void *>(&g->shm->writer), sizeof(vrmv_writer_block_t));
    if (out_generation) {
      *out_generation = g->generation;
    }
    return true;
  }

}  // namespace vr::mv_bridge
