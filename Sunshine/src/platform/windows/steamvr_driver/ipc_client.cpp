// ipc_client.cpp - 見 ipc_client.h。狀態機對照設計 §B.6：
//   DISCONNECTED → (CreateFileW) → VERIFY_SERVER → HELLO_SENT → MAPPED → OPERATIONAL → TEARDOWN → DISCONNECTED
//   REJECT(ABI) → DEAD（本行程不再重連）
#include "ipc_client.h"

#include <aclapi.h>
#include <process.h>

#include <cstdio>
#include <cstring>

#include "driver_log.h"
#include "ipc_proto.h"

namespace vrdrv {
  namespace {

    int64_t qpc_now() {
      LARGE_INTEGER v;
      QueryPerformanceCounter(&v);
      return v.QuadPart;
    }

    int64_t ms_to_qpc(int64_t qpf, int64_t ms) {
      return (qpf / 1000) * ms + ((qpf % 1000) * ms) / 1000;
    }

    int64_t qpc_to_ms_ceil(int64_t qpf, int64_t ticks) {
      if (ticks <= 0) {
        return 0;
      }
      return (ticks * 1000 + qpf - 1) / qpf;
    }

    HANDLE to_handle(uint64_t v) {
      return (HANDLE) (uintptr_t) v;
    }

    // WELCOME／TEXTURES 帶來的 handle：送達即歸本行程所有，值合理才關（server 是可信方；
    // 值不合理代表訊息本身壞了，關一個不屬於我們的值比洩漏更糟）。
    void close_if_plausible(uint64_t v) {
      if (proto::plausible_handle_value(v)) {
        CloseHandle(to_handle(v));
      }
    }

    // ── 選配的 server 映像檢查（§B.1）：NtQuerySystemInformation(SystemProcessIdInformation) ──
    // 以 GetProcAddress 取得，driver 的匯入表不會出現 ntdll.dll（Stage 的 dumpbin 白名單）。
    struct vr_unicode_string_t {
      USHORT Length;
      USHORT MaximumLength;
      PWSTR Buffer;
    };

    struct vr_process_id_info_t {
      HANDLE ProcessId;
      vr_unicode_string_t ImageName;
    };

    using nt_query_system_information_t = LONG(NTAPI *)(ULONG, PVOID, ULONG, PULONG);
    constexpr ULONG k_system_process_id_information = 0x58;

    bool query_process_image_nt(DWORD pid, std::wstring &out) {
      HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
      if (!ntdll) {
        return false;
      }
      auto fn = reinterpret_cast<nt_query_system_information_t>(reinterpret_cast<void *>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
      if (!fn) {
        return false;
      }
      std::wstring buf(32767, L'\0');
      vr_process_id_info_t info {};
      info.ProcessId = (HANDLE) (uintptr_t) pid;
      info.ImageName.Length = 0;
      info.ImageName.MaximumLength = (USHORT) (32767 * sizeof(wchar_t));
      info.ImageName.Buffer = buf.data();
      const LONG st = fn(k_system_process_id_information, &info, (ULONG) sizeof(info), nullptr);
      if (st < 0) {
        return false;
      }
      out.assign(buf.data(), info.ImageName.Length / sizeof(wchar_t));
      return !out.empty();
    }

    // "C:\a\b.exe" → "\Device\HarddiskVolumeN\a\b.exe"；只接受磁碟機代號開頭的絕對路徑。
    bool dos_to_nt_path(const std::wstring &dos_in, std::wstring &out) {
      std::wstring dos = dos_in;
      if (dos.rfind(L"\\\\?\\", 0) == 0) {
        dos = dos.substr(4);
      }
      if (dos.size() < 3 || dos[1] != L':' || (dos[2] != L'\\' && dos[2] != L'/')) {
        return false;
      }
      wchar_t drive[3] = {dos[0], L':', L'\0'};
      wchar_t dev[MAX_PATH] {};
      if (QueryDosDeviceW(drive, dev, MAX_PATH) == 0) {
        return false;
      }
      out = dev;
      out += dos.substr(2);
      return true;
    }

    std::wstring normalize_slashes(std::wstring s) {
      for (auto &ch : s) {
        if (ch == L'/') {
          ch = L'\\';
        }
      }
      return s;
    }

    bool path_equal_ci(const std::wstring &a, const std::wstring &b) {
      const std::wstring na = normalize_slashes(a);
      const std::wstring nb = normalize_slashes(b);
      return CompareStringOrdinal(na.c_str(), (int) na.size(), nb.c_str(), (int) nb.size(), TRUE) == CSTR_EQUAL;
    }

    const char *reject_name(uint32_t r) {
      switch (r) {
        case VRIPC_REJ_ABI_MISMATCH:
          return "abi-mismatch";
        case VRIPC_REJ_IDENTITY:
          return "identity";
        case VRIPC_REJ_BAD_MESSAGE:
          return "bad-message";
        case VRIPC_REJ_ADAPTER_MISMATCH:
          return "adapter-mismatch";
        case VRIPC_REJ_TEXTURE_INVALID:
          return "texture-invalid";
        case VRIPC_REJ_BUSY:
          return "busy";
        case VRIPC_REJ_VR_DISABLED:
          return "vr-disabled";
        case VRIPC_REJ_TIMEOUT:
          return "timeout";
        case VRIPC_REJ_GENERATION:
          return "generation";
        default:
          return "unknown";
      }
    }

    const char *bye_name(uint32_t r) {
      switch (r) {
        case VRIPC_BYE_NORMAL:
          return "normal";
        case VRIPC_BYE_RECONFIG:
          return "reconfig";
        case VRIPC_BYE_SERVER_SHUTDOWN:
          return "server-shutdown";
        case VRIPC_BYE_DRIVER_CLEANUP:
          return "driver-cleanup";
        case VRIPC_BYE_PROTOCOL_ERROR:
          return "protocol-error";
        case VRIPC_BYE_DEVICE_LOST:
          return "device-lost";
        default:
          return "unknown";
      }
    }

    constexpr uint32_t k_use_backoff = 0xFFFFFFFFu;  // session_end_t.delay_ms：依連續失敗次數退避
  }  // namespace

  // ════════════════════════════════════════════════════════════════════════
  // generation_t
  // ════════════════════════════════════════════════════════════════════════
  generation_t::~generation_t() {
    if (shm_) {
      UnmapViewOfFile(shm_);
      shm_ = nullptr;
    }
    if (evt_trk_) {
      CloseHandle(evt_trk_);
      evt_trk_ = nullptr;
    }
    if (evt_frm_) {
      CloseHandle(evt_frm_);
      evt_frm_ = nullptr;
    }
  }

  bool generation_t::read_latest_tracking(vripc_tracking_slot_t &out, uint64_t &idx, bool *torn) {
    const auto r = proto::seq_read_latest<vripc_tracking_slot_t, VRIPC_TRK_SLOTS>(&shm_->tracking.write_index, shm_->tracking.slot, out, idx);
    if (torn) {
      *torn = (r == proto::seq_read_e::torn);
    }
    if (r == proto::seq_read_e::torn) {
      tracking_torn_.fetch_add(1, std::memory_order_relaxed);
    }
    return r == proto::seq_read_e::ok;
  }

  bool generation_t::read_pacing(vripc_pacing_t &out) {
    return proto::seq_read_single(&shm_->pacing, out);
  }

  void generation_t::read_header(vripc_shm_header_t &out) const {
    VRIPC_COMPILER_BARRIER();
    std::memcpy(&out, (const void *) &shm_->header, sizeof(out));
    VRIPC_COMPILER_BARRIER();
  }

  void generation_t::read_consume(vripc_consume_status_t &out) const {
    VRIPC_COMPILER_BARRIER();
    std::memcpy(&out, (const void *) &shm_->consume, sizeof(out));
    VRIPC_COMPILER_BARRIER();
  }

  void generation_t::publish_frame(const vripc_frame_desc_t &in) {
    {
      std::lock_guard<std::mutex> lk(frames_mtx_);
      proto::seq_write<vripc_frame_desc_t, VRIPC_FRM_SLOTS>(&shm_->frames.write_index, shm_->frames.slot, frames_local_index_, in);
    }
    frames_published_.fetch_add(1, std::memory_order_relaxed);
    if (evt_frm_) {
      SetEvent(evt_frm_);
    }
  }

  bool generation_t::push_haptic(const vripc_haptic_evt_t &evt) {
    std::lock_guard<std::mutex> lk(haptic_mtx_);
    const auto r = proto::spsc_push<vripc_haptic_evt_t, VRIPC_HAPTIC_SLOTS>(&shm_->haptic.head, &shm_->haptic.tail, shm_->haptic.evt, haptic_local_head_, evt);
    if (r == proto::spsc_push_e::ok) {
      return true;
    }
    haptic_dropped_.fetch_add(1, std::memory_order_relaxed);
    if (r == proto::spsc_push_e::corrupt) {
      ring_corrupt_.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
  }

  bool generation_t::push_timing(const vripc_timing_rec_t &rec) {
    std::lock_guard<std::mutex> lk(timing_mtx_);
    const auto r = proto::spsc_push<vripc_timing_rec_t, VRIPC_TIMING_SLOTS>(&shm_->timing.head, &shm_->timing.tail, shm_->timing.rec, timing_local_head_, rec);
    if (r == proto::spsc_push_e::ok) {
      return true;
    }
    timing_dropped_.fetch_add(1, std::memory_order_relaxed);
    if (r == proto::spsc_push_e::corrupt) {
      ring_corrupt_.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
  }

  bool generation_t::push_log(uint32_t level, const char *text) {
    std::lock_guard<std::mutex> lk(log_mtx_);
    vripc_log_line_t line {};
    line.qpc = qpc_now();
    line.level = level > 2 ? 2 : level;
    if (log_pending_dropped_ != 0) {
      char tmp[VRIPC_LOG_TEXT + 32];
      snprintf(tmp, sizeof(tmp), "(dropped %llu) %s", (unsigned long long) log_pending_dropped_, text ? text : "");
      line.len = proto::make_log_text(line.text, tmp);
    } else {
      line.len = proto::make_log_text(line.text, text);
    }
    const auto r = proto::spsc_push<vripc_log_line_t, VRIPC_LOG_SLOTS>(&shm_->log.head, &shm_->log.tail, shm_->log.line, log_local_head_, line);
    if (r == proto::spsc_push_e::ok) {
      log_pending_dropped_ = 0;
      return true;
    }
    ++log_pending_dropped_;
    log_dropped_.fetch_add(1, std::memory_order_relaxed);
    if (r == proto::spsc_push_e::corrupt) {
      ring_corrupt_.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
  }

  generation_t::counters_t generation_t::counters() const {
    counters_t c {};
    c.frames_published = frames_published_.load(std::memory_order_relaxed);
    c.haptic_dropped = haptic_dropped_.load(std::memory_order_relaxed);
    c.timing_dropped = timing_dropped_.load(std::memory_order_relaxed);
    c.log_dropped = log_dropped_.load(std::memory_order_relaxed);
    c.ring_corrupt = ring_corrupt_.load(std::memory_order_relaxed);
    c.tracking_torn = tracking_torn_.load(std::memory_order_relaxed);
    return c;
  }

  // ════════════════════════════════════════════════════════════════════════
  // session（一次 pipe 連線）
  // ════════════════════════════════════════════════════════════════════════
  struct ipc_client_t::session_t {
    enum class st_e {
      hello_sent,
      mapped,
      operational,
    };

    HANDLE pipe = INVALID_HANDLE_VALUE;
    OVERLAPPED rov {};
    OVERLAPPED wov {};
    alignas(8) uint8_t rbuf[VRIPC_PIPE_MAX_MSG] {};
    bool read_pending = false;
    bool pipe_broken = false;
    bool never_read = false;
    uint32_t seq_out = 0;
    uint32_t last_seq_in = 0;
    st_e st = st_e::hello_sent;
    generation_ptr gen;
    uint32_t link_state = 0;
    int64_t deadline_qpc = 0;  // 0 = 沒有
    int64_t next_hb_qpc = 0;
    int64_t mapped_qpc = 0;
    bool server_stale = false;
    bool textures_done = false;
    bool header_bad_logged = false;

    session_t() {
      rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      wov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~session_t() {
      if (rov.hEvent) {
        CloseHandle(rov.hEvent);
      }
      if (wov.hEvent) {
        CloseHandle(wov.hEvent);
      }
    }

    session_t(const session_t &) = delete;
    session_t &operator=(const session_t &) = delete;

    bool ok() const {
      return rov.hEvent && wov.hEvent;
    }

    // 送一則訊息：header 的 seq 在這裡填；overlapped、100 ms 逾時 → CancelIoEx → 失敗。
    // 絕不呼叫 FlushFileBuffers（§B.4、sec-M8）。
    enum class wr_e {
      ok,
      timeout,
      error,
    };

    wr_e write_raw(void *msg, uint32_t size) {
      if (pipe_broken) {
        return wr_e::error;
      }
      auto *h = static_cast<vripc_msg_hdr_t *>(msg);
      h->seq = ++seq_out;
      ResetEvent(wov.hEvent);
      if (!WriteFile(pipe, msg, size, nullptr, &wov)) {
        const DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
          pipe_broken = true;
          return wr_e::error;
        }
      }
      if (WaitForSingleObject(wov.hEvent, proto::k_write_timeout_ms) != WAIT_OBJECT_0) {
        CancelIoEx(pipe, &wov);
        DWORD n = 0;
        GetOverlappedResult(pipe, &wov, &n, TRUE);  // 等取消完成（對 pipe 是立即的）
        return wr_e::timeout;
      }
      DWORD n = 0;
      if (!GetOverlappedResult(pipe, &wov, &n, FALSE) || n != size) {
        pipe_broken = true;
        return wr_e::error;
      }
      return wr_e::ok;
    }

    bool post_read() {
      if (pipe_broken || never_read) {
        return false;
      }
      ResetEvent(rov.hEvent);
      if (!ReadFile(pipe, rbuf, sizeof(rbuf), nullptr, &rov)) {
        const DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING && e != ERROR_MORE_DATA) {
          pipe_broken = true;
          return false;
        }
      }
      read_pending = true;
      return true;
    }

    void cancel_read() {
      if (!read_pending) {
        return;
      }
      CancelIoEx(pipe, &rov);
      DWORD n = 0;
      GetOverlappedResult(pipe, &rov, &n, TRUE);
      read_pending = false;
    }
  };

  // ════════════════════════════════════════════════════════════════════════
  // ipc_client_t
  // ════════════════════════════════════════════════════════════════════════
  ipc_client_t::~ipc_client_t() {
    // 正常流程是呼叫端先 stop() 成功。這裡是最後防線：執行緒還活著時不可釋放記憶體，
    // 所以等到它結束為止（ipc 執行緒的每個阻塞點都有上限，實務上 < 300 ms）。
    if (thread_) {
      if (!stop(500)) {
        WaitForSingleObject(thread_, INFINITE);
        CloseHandle(thread_);
        thread_ = nullptr;
      }
    }
    if (stop_evt_) {
      CloseHandle(stop_evt_);
      stop_evt_ = nullptr;
    }
    if (first_evt_) {
      CloseHandle(first_evt_);
      first_evt_ = nullptr;
    }
    if (send_evt_) {
      CloseHandle(send_evt_);
      send_evt_ = nullptr;
    }
  }

  bool ipc_client_t::start(const ipc_client_config_t &cfg, const ipc_callbacks_t &cb) {
    if (thread_) {
      return false;
    }
    cfg_ = cfg;
    cb_ = cb;
    if (cfg_.pipe_name.empty()) {
      DWORD session = 0;
      if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) {
        VRDRV_LOG_ERROR("ipc start-failed reason=session-id err=%lu", GetLastError());
        return false;
      }
      cfg_.pipe_name = VRIPC_PIPE_NAME_PREFIX + std::to_wstring(session);
    }
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpf_ = f.QuadPart;
    if (!stop_evt_) {
      stop_evt_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (!first_evt_) {
      first_evt_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    if (!send_evt_) {
      send_evt_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    if (!stop_evt_ || !first_evt_ || !send_evt_) {
      VRDRV_LOG_ERROR("ipc start-failed reason=create-event err=%lu", GetLastError());
      return false;
    }
    ResetEvent(stop_evt_);
    ResetEvent(first_evt_);
    first_.store((int) first_e::pending);
    thread_ = (HANDLE) _beginthreadex(nullptr, 0, &ipc_client_t::thread_entry, this, 0, nullptr);
    if (!thread_) {
      VRDRV_LOG_ERROR("ipc start-failed reason=thread");
      return false;
    }
    return true;
  }

  ipc_client_t::first_e ipc_client_t::wait_first(uint32_t timeout_ms) {
    if (!first_evt_) {
      return first_e::failed;
    }
    WaitForSingleObject(first_evt_, timeout_ms);
    return (first_e) first_.load();
  }

  bool ipc_client_t::stop(uint32_t join_timeout_ms) {
    if (!thread_) {
      return true;
    }
    SetEvent(stop_evt_);
    if (WaitForSingleObject(thread_, join_timeout_ms) != WAIT_OBJECT_0) {
      VRDRV_LOG_WARN("ipc stop join-timeout ms=%u", join_timeout_ms);
      return false;
    }
    CloseHandle(thread_);
    thread_ = nullptr;
    return true;
  }

  generation_ptr ipc_client_t::current() {
    std::lock_guard<std::mutex> lk(gen_mtx_);
    return gen_;
  }

  void ipc_client_t::send_state(uint32_t kind, uint32_t arg) {
    if (!current()) {
      std::lock_guard<std::mutex> lk(send_mtx_);
      ++send_dropped_;
      return;
    }
    {
      std::lock_guard<std::mutex> lk(send_mtx_);
      if (send_count_ >= k_send_queue) {
        ++send_dropped_;
        return;
      }
      send_queue_[send_count_++] = {kind, arg};
    }
    if (send_evt_) {
      SetEvent(send_evt_);
    }
  }

  void ipc_client_t::report_device_lost(uint32_t hresult) {
    device_lost_hr_.store(hresult);
    device_lost_pending_.store(true);
    if (send_evt_) {
      SetEvent(send_evt_);
    }
  }

  void ipc_client_t::set_device_state(uint32_t drv_state) {
    device_state_.store(drv_state);
  }

  void ipc_client_t::set_activated(const activated_params_t &p) {
    std::lock_guard<std::mutex> lk(status_mtx_);
    activated_ = p;
  }

  void ipc_client_t::set_other_hmd_system(const char *name) {
    std::lock_guard<std::mutex> lk(status_mtx_);
    std::memset(other_hmd_system_, 0, sizeof(other_hmd_system_));
    if (!name) {
      return;
    }
    for (size_t i = 0; i < sizeof(other_hmd_system_) - 1 && name[i] != '\0'; ++i) {
      const unsigned char ch = (unsigned char) name[i];
      other_hmd_system_[i] = (ch >= 0x20 && ch < 0x7F && ch != '[') ? (char) ch : '?';
    }
  }

  ipc_client_t::stats_t ipc_client_t::stats() const {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    return stats_;
  }

  std::wstring ipc_client_t::expected_server_image_from_module(HMODULE module) {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, path.data(), (DWORD) path.size());
    if (n == 0 || n >= path.size()) {
      return {};
    }
    path.resize(n);
    return expected_server_image_from_dll_path(path);
  }

  std::wstring ipc_client_t::expected_server_image_from_dll_path(const std::wstring &dll_path) {
    std::wstring path = normalize_slashes(dll_path);
    // 去掉檔名 → <...>\bin\win64；再往上 6 層：bin\win64 → viplestream → <ver> → steamvr → config → <install>
    for (int i = 0; i < 7; ++i) {
      const size_t pos = path.find_last_of(L'\\');
      if (pos == std::wstring::npos || pos == 0) {
        return {};
      }
      path.resize(pos);
    }
    return path + L"\\viplestream-server.exe";
  }

  unsigned __stdcall ipc_client_t::thread_entry(void *self) {
    static_cast<ipc_client_t *>(self)->run();
    return 0;
  }

  void ipc_client_t::signal_first(first_e v) {
    int expected = (int) first_e::pending;
    if (first_.compare_exchange_strong(expected, (int) v)) {
      SetEvent(first_evt_);
    }
  }

  // FILE_NOT_FOUND／ACCESS_DENIED 等「server 還沒開放」的狀況每 60 s 最多記一次（§B.6）。
  void ipc_client_t::log_connect_problem(const char *what, uint32_t err) {
    const int64_t now = qpc_now();
    if (last_quiet_log_qpc_ != 0 && now - last_quiet_log_qpc_ < ms_to_qpc(qpf_, proto::k_quiet_log_ms)) {
      ++quiet_suppressed_;
      return;
    }
    last_quiet_log_qpc_ = now;
    VRDRV_LOG_INFO("ipc-connect-failed what=%s err=%u quiet=%u", what, err, quiet_suppressed_);
    quiet_suppressed_ = 0;
  }

  ipc_client_t::connect_e ipc_client_t::connect_once(HANDLE &pipe, const char *&why, uint32_t &err) {
    why = "ok";
    err = 0;
    pipe = CreateFileW(cfg_.pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
      err = GetLastError();
      if (err == ERROR_FILE_NOT_FOUND) {
        why = "not-found";
        return connect_e::not_found;
      }
      if (err == ERROR_ACCESS_DENIED) {
        why = "access-denied";
        return connect_e::not_found;
      }
      if (err == ERROR_PIPE_BUSY) {
        why = "busy";
        return connect_e::busy;
      }
      why = "create-file";
      return connect_e::error;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr)) {
      err = GetLastError();
      CloseHandle(pipe);
      pipe = INVALID_HANDLE_VALUE;
      why = "set-message-mode";
      return connect_e::error;
    }
    bool trusted = false;
#if defined(VRDRV_PEER_HOOKS)
    if (cfg_.peer_mode && cfg_.hooks.verify_server_override) {
      trusted = cfg_.hooks.verify_server_override(pipe, why);
    } else {
      trusted = verify_server(pipe, why, err);
    }
#else
    trusted = verify_server(pipe, why, err);
#endif
    if (!trusted) {
      CloseHandle(pipe);
      pipe = INVALID_HANDLE_VALUE;
      return connect_e::untrusted;
    }
    return connect_e::ok;
  }

  // driver 驗 server（§B.1、K4）：擁有者 S-1-5-18、DACL protected、同一個 session；選配映像路徑。
  bool ipc_client_t::verify_server(HANDLE pipe, const char *&why, uint32_t &err) {
    PSID owner = nullptr;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    const DWORD rc = GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &sd);
    if (rc != ERROR_SUCCESS) {
      why = "secinfo";
      err = rc;
      return false;
    }
    const bool owner_system = owner && IsValidSid(owner) && IsWellKnownSid(owner, WinLocalSystemSid);
    SECURITY_DESCRIPTOR_CONTROL ctl = 0;
    DWORD rev = 0;
    const bool ctl_ok = GetSecurityDescriptorControl(sd, &ctl, &rev) != FALSE;
    const bool has_dacl = dacl != nullptr;
    LocalFree(sd);
    if (!owner_system) {
      why = "owner";
      return false;
    }
    if (!ctl_ok || (ctl & SE_DACL_PROTECTED) == 0) {
      why = "dacl-not-protected";
      return false;
    }
    if (!has_dacl) {
      why = "dacl-null";
      return false;
    }
    ULONG server_session = 0;
    if (!GetNamedPipeServerSessionId(pipe, &server_session)) {
      why = "server-session-query";
      err = GetLastError();
      return false;
    }
    DWORD own_session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &own_session)) {
      why = "own-session-query";
      err = GetLastError();
      return false;
    }
    if (server_session != own_session) {
      why = "session";
      return false;
    }
    if (!cfg_.peer_mode && !cfg_.expected_server_image.empty()) {
      ULONG server_pid = 0;
      if (!GetNamedPipeServerProcessId(pipe, &server_pid)) {
        why = "server-pid-query";
        err = GetLastError();
        return false;
      }
      std::wstring actual;
      std::wstring expected;
      if (!query_process_image_nt(server_pid, actual)) {
        why = "image-query";
        return false;
      }
      if (!dos_to_nt_path(cfg_.expected_server_image, expected)) {
        why = "image-expected";
        return false;
      }
      if (!path_equal_ci(actual, expected)) {
        why = "image";
        return false;
      }
    }
    why = "ok";
    return true;
  }

  void ipc_client_t::write_status_block(const generation_ptr &gen, uint32_t link_state) {
    if (!gen || !gen->shm_) {
      return;
    }
    vripc_driver_status_t d {};
    d.generation_ack = gen->id();
    d.driver_pid = GetCurrentProcessId();
    const uint32_t dev = device_state_.load();
    d.driver_state = dev != 0 ? dev : link_state;
    d.frames_presented = status_.frames_presented.load(std::memory_order_relaxed);
    d.frames_composited = status_.frames_composited.load(std::memory_order_relaxed);
    d.drop_noslot = status_.drop_noslot.load(std::memory_order_relaxed);
    d.drop_acquire_timeout = status_.drop_acquire_timeout.load(std::memory_order_relaxed);
    d.drop_format = status_.drop_format.load(std::memory_order_relaxed);
    d.stale_pose_count = status_.stale_pose_count.load(std::memory_order_relaxed);
    d.outofrange_count = status_.outofrange_count.load(std::memory_order_relaxed);
    d.posehist_miss = status_.posehist_miss.load(std::memory_order_relaxed);
    d.last_vsync_qpc = status_.last_vsync_qpc.load(std::memory_order_relaxed);
    d.swapsets_live = status_.swapsets_live.load(std::memory_order_relaxed);
    d.last_layer_count = status_.last_layer_count.load(std::memory_order_relaxed);
    d.space_delta_mdeg = status_.space_delta_mdeg.load(std::memory_order_relaxed);
    d.other_hmd = status_.other_hmd.load(std::memory_order_relaxed);
    d.degraded_code = status_.degraded_code.load(std::memory_order_relaxed);
    d.next_index_calls = status_.next_index_calls.load(std::memory_order_relaxed);
    d.drop_unknown_layer = status_.drop_unknown_layer.load(std::memory_order_relaxed);
    d.max_layer_count = status_.max_layer_count.load(std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(status_mtx_);
      std::memcpy(d.other_hmd_system, other_hmd_system_, sizeof(d.other_hmd_system));
      d.act_refresh_mhz = activated_.refresh_mhz;
      d.act_eye_width = activated_.eye_width;
      d.act_eye_height = activated_.eye_height;
      d.act_luid_low = activated_.luid_low;
      d.act_luid_high = activated_.luid_high;
    }
    d.driver_heartbeat_qpc = qpc_now();
    // 只有 ipc 執行緒寫這個區塊（單一寫入方）；server 整份複製後驗證，不需要 seqlock。
    VRIPC_COMPILER_BARRIER();
    std::memcpy((void *) &gen->shm_->driver, &d, sizeof(d));
    VRIPC_COMPILER_BARRIER();
  }

  void ipc_client_t::run() {
    uint32_t failures = 0;
    uint32_t delay = 0;
    for (;;) {
      if (WaitForSingleObject(stop_evt_, delay) == WAIT_OBJECT_0) {
        break;
      }
      HANDLE pipe = INVALID_HANDLE_VALUE;
      const char *why = "";
      uint32_t err = 0;
      const connect_e c = connect_once(pipe, why, err);
#if defined(VRDRV_PEER_HOOKS)
      if (c != connect_e::ok && cfg_.peer_mode && cfg_.hooks.on_attempt) {
        cfg_.hooks.on_attempt(why, err);
      }
#endif
      if (c != connect_e::ok) {
        switch (c) {
          case connect_e::not_found:
            ++failures;
            delay = proto::backoff_ms(failures);
            log_connect_problem(why, err);
            signal_first(first_e::no_server);
            break;
          case connect_e::busy:
            {
              // §B.4 D1：PIPE_BUSY（server 還在拆上一條連線、或別的 client 連著）→ WaitNamedPipe 後重試，不累計失敗。
              // server 一回到 ConnectNamedPipe 就能立刻連上（RECONFIG／DEVICE_LOST 的「立刻重連」不必多等 500 ms）。
              // 每次最多等 100 ms，讓 stop() 的 join 上限（500 ms）仍然成立；可用之後稍等 10 ms，避免與別的 client 搶成忙迴圈。
              const BOOL available = WaitNamedPipeW(cfg_.pipe_name.c_str(), 100);
              delay = available ? 10 : 0;
              log_connect_problem(why, err);
              break;
            }
          case connect_e::untrusted:
            ++failures;
            delay = proto::k_untrusted_backoff_ms;
            VRDRV_LOG_WARN("ipc-server-untrusted reason=%s err=%u", why, err);
            signal_first(first_e::untrusted);
            break;
          default:
            ++failures;
            delay = proto::backoff_ms(failures);
            VRDRV_LOG_WARN("ipc-connect-failed what=%s err=%u", why, err);
            signal_first(first_e::failed);
            break;
        }
#if defined(VRDRV_PEER_HOOKS)
        if (cfg_.peer_mode && cfg_.hooks.fixed_retry_ms != 0) {
          delay = cfg_.hooks.fixed_retry_ms;
        }
#endif
        continue;
      }
      {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        ++stats_.connects;
      }
      const session_end_t e = run_session(pipe);
      if (e.stop) {
        break;
      }
      bool dead = e.dead;
#if defined(VRDRV_PEER_HOOKS)
      if (dead && cfg_.peer_mode && cfg_.hooks.ignore_dead) {
        dead = false;
      }
#endif
      if (dead) {
        {
          std::lock_guard<std::mutex> lk(stats_mtx_);
          stats_.dead = true;
        }
        signal_first(first_e::dead);
        WaitForSingleObject(stop_evt_, INFINITE);
        break;
      }
      if (e.reset_failures) {
        failures = 0;
      }
      if (e.delay_ms == k_use_backoff) {
        ++failures;
        delay = proto::backoff_ms(failures);
      } else {
        delay = e.delay_ms;
      }
#if defined(VRDRV_PEER_HOOKS)
      if (cfg_.peer_mode && cfg_.hooks.fixed_retry_ms != 0) {
        delay = cfg_.hooks.fixed_retry_ms;
      }
#endif
    }
  }

  ipc_client_t::session_end_t ipc_client_t::run_session(HANDLE pipe) {
    session_t s;
    s.pipe = pipe;
    uint64_t teardown_gen = 0;

    // 共用的收尾（§B.6 TEARDOWN）：先取消 pending read、需要時送 BYE、清掉 generation、關 pipe。
    auto teardown = [&](const char *reason, int bye_reason) {
      s.cancel_read();
      if (bye_reason >= 0 && !s.pipe_broken) {
        vripc_bye_t bye {};
        bye.hdr = proto::make_header(VRIPC_MSG_BYE, (uint32_t) sizeof(bye), 0);
        bye.reason = (uint32_t) bye_reason;
        s.write_raw(&bye, (uint32_t) sizeof(bye));
      }
      if (s.gen) {
        teardown_gen = s.gen->id();
        {
          std::lock_guard<std::mutex> lk(gen_mtx_);
          gen_.reset();
        }
        if (cb_.on_teardown) {
          cb_.on_teardown(teardown_gen, reason);
        }
        s.gen.reset();  // 其他執行緒放掉最後一個參照時才 unmap
      }
      {
        std::lock_guard<std::mutex> lk(send_mtx_);
        send_count_ = 0;
      }
      CloseHandle(s.pipe);
      s.pipe = INVALID_HANDLE_VALUE;
      {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        ++stats_.teardowns;
      }
      VRDRV_LOG_INFO("ipc teardown gen=%llu reason=%s", (unsigned long long) teardown_gen, reason);
    };

    auto protocol_error = [&](const char *what) -> session_end_t {
      {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        ++stats_.protocol_errors;
      }
      VRDRV_LOG_WARN("ipc protocol-error what=%s", what);
      teardown("protocol-error", VRIPC_BYE_PROTOCOL_ERROR);
      return {k_use_backoff, false, false, false};
    };

    auto send_reject = [&](uint32_t reason, uint32_t detail) {
      vripc_reject_t rj {};
      rj.hdr = proto::make_header(VRIPC_MSG_REJECT, (uint32_t) sizeof(rj), 0);
      rj.reason = reason;
      rj.detail = detail;
      s.write_raw(&rj, (uint32_t) sizeof(rj));
    };

#if defined(VRDRV_PEER_HOOKS)
    auto attempt_hook = [&](const char *outcome, uint32_t detail) {
      if (cfg_.peer_mode && cfg_.hooks.on_attempt) {
        cfg_.hooks.on_attempt(outcome, detail);
      }
    };
#else
    auto attempt_hook = [](const char *, uint32_t) {};
#endif

    if (!s.ok()) {
      CloseHandle(pipe);
      VRDRV_LOG_ERROR("ipc session-failed reason=create-event");
      return {k_use_backoff, false, false, false};
    }

    // ── D3：HELLO ──
    {
      alignas(8) uint8_t buf[VRIPC_PIPE_MAX_MSG] {};
      vripc_hello_t hello {};
      uint32_t size = (uint32_t) sizeof(hello);
      uint32_t abi = VRIPC_ABI_VERSION;
#if defined(VRDRV_PEER_HOOKS)
      if (cfg_.peer_mode) {
        abi = cfg_.hooks.hello_abi;
        size = cfg_.hooks.hello_size;
        if (size < proto::k_min_msg) {
          size = proto::k_min_msg;
        }
        if (size > VRIPC_PIPE_MAX_MSG) {
          size = VRIPC_PIPE_MAX_MSG;
        }
      }
#endif
      hello.hdr = proto::make_header(VRIPC_MSG_HELLO, size, 0);
      hello.abi_version = abi;
      hello.driver_pid = GetCurrentProcessId();
      hello.driver_version_packed = cfg_.driver_version_packed;
      hello.shm_struct_size = (uint32_t) sizeof(vripc_shm_t);
      hello.reserved_mb = 0;
      hello.driver_caps = cfg_.driver_caps | (cfg_.peer_mode ? VRIPC_DCAP_PEER : 0u);
      hello.openvr_sdk_packed = cfg_.openvr_sdk_packed;
      const size_t n = cfg_.iface_directmode.size() < VRIPC_IFACE_CHARS - 1 ? cfg_.iface_directmode.size() : VRIPC_IFACE_CHARS - 1;
      std::memcpy(hello.iface_directmode, cfg_.iface_directmode.data(), n);
      std::memcpy(buf, &hello, size < sizeof(hello) ? size : sizeof(hello));
      if (s.write_raw(buf, size) != session_t::wr_e::ok) {
        attempt_hook("hello-write-failed", 0);
        teardown("hello-write-failed", -1);
        return {k_use_backoff, false, false, false};
      }
    }
    if (!s.post_read()) {
      attempt_hook("disconnect", 0);
      teardown("pipe-broken", -1);
      return {k_use_backoff, false, false, false};
    }
    s.deadline_qpc = qpc_now() + ms_to_qpc(qpf_, proto::k_hello_timeout_ms);

    for (;;) {
      const int64_t now = qpc_now();
      int64_t wait_ms = 100;
      if (s.deadline_qpc != 0) {
        const int64_t d = qpc_to_ms_ceil(qpf_, s.deadline_qpc - now);
        wait_ms = d < wait_ms ? d : wait_ms;
      }
      if (s.gen) {
        const int64_t d = qpc_to_ms_ceil(qpf_, s.next_hb_qpc - now);
        wait_ms = d < wait_ms ? d : wait_ms;
      }
      HANDLE hs[3];
      DWORD nh = 0;
      hs[nh++] = stop_evt_;
      hs[nh++] = send_evt_;
      if (s.read_pending) {
        hs[nh++] = s.rov.hEvent;
      }
      const DWORD w = WaitForMultipleObjects(nh, hs, FALSE, (DWORD) (wait_ms < 0 ? 0 : wait_ms));

      // ── 停止（Cleanup）：先送還沒送的 STATE（例如 EXITING），再取消 read、BYE(DRIVER_CLEANUP) ──
      if (w == WAIT_OBJECT_0) {
        if (s.gen) {
          pending_state_t q[k_send_queue];
          uint32_t cnt = 0;
          {
            std::lock_guard<std::mutex> lk(send_mtx_);
            cnt = send_count_;
            std::memcpy(q, send_queue_, sizeof(pending_state_t) * cnt);
            send_count_ = 0;
          }
          write_status_block(s.gen, s.link_state);
          for (uint32_t i = 0; i < cnt; ++i) {
            vripc_state_t m {};
            m.hdr = proto::make_header(VRIPC_MSG_STATE, (uint32_t) sizeof(m), 0);
            m.generation = s.gen->id();
            m.kind = q[i].kind;
            m.arg = q[i].arg;
            if (s.write_raw(&m, (uint32_t) sizeof(m)) != session_t::wr_e::ok) {
              break;
            }
          }
        }
        teardown("cleanup", VRIPC_BYE_DRIVER_CLEANUP);
        return {0, false, true, false};
      }

      // ── STATE 佇列 ──
      if (w == WAIT_OBJECT_0 + 1 && s.gen) {
        pending_state_t q[k_send_queue];
        uint32_t cnt = 0;
        {
          std::lock_guard<std::mutex> lk(send_mtx_);
          cnt = send_count_;
          std::memcpy(q, send_queue_, sizeof(pending_state_t) * cnt);
          send_count_ = 0;
        }
        if (cnt != 0) {
          // act_* 等欄位要在 STATE 之前就在 shm 裡（VRIPC_ST_HMD_ACTIVATED 的約定）
          write_status_block(s.gen, s.link_state);
        }
        for (uint32_t i = 0; i < cnt; ++i) {
          vripc_state_t m {};
          m.hdr = proto::make_header(VRIPC_MSG_STATE, (uint32_t) sizeof(m), 0);
          m.generation = s.gen->id();
          m.kind = q[i].kind;
          m.arg = q[i].arg;
          const auto r = s.write_raw(&m, (uint32_t) sizeof(m));
          if (r != session_t::wr_e::ok) {
            if (r == session_t::wr_e::timeout) {
              std::lock_guard<std::mutex> lk(stats_mtx_);
              ++stats_.write_timeouts;
            }
            teardown(r == session_t::wr_e::timeout ? "write-timeout" : "pipe-broken", -1);
            return {k_use_backoff, false, false, false};
          }
        }
      }

      // ── device lost（§B.6）：STATE DEVICE_LOST → BYE(DEVICE_LOST) → TEARDOWN → 立即重連 ──
      if (device_lost_pending_.exchange(false)) {
        if (s.gen) {
          const uint32_t hr = device_lost_hr_.load();
          VRDRV_LOG_WARN("ipc device-lost gen=%llu hr=0x%08x", (unsigned long long) s.gen->id(), hr);
          write_status_block(s.gen, s.link_state);
          vripc_state_t m {};
          m.hdr = proto::make_header(VRIPC_MSG_STATE, (uint32_t) sizeof(m), 0);
          m.generation = s.gen->id();
          m.kind = VRIPC_ST_DEVICE_LOST;
          m.arg = hr;
          s.write_raw(&m, (uint32_t) sizeof(m));
          teardown("device-lost", VRIPC_BYE_DEVICE_LOST);
          return {0, false, false, true};
        }
      }

      // ── 收到訊息 ──
      if (s.read_pending && WaitForSingleObject(s.rov.hEvent, 0) == WAIT_OBJECT_0) {
        DWORD n = 0;
        const BOOL ok = GetOverlappedResult(s.pipe, &s.rov, &n, FALSE);
        s.read_pending = false;
        if (!ok) {
          const DWORD e = GetLastError();
          if (e == ERROR_MORE_DATA) {
            return protocol_error("oversize");
          }
          s.pipe_broken = true;
          const bool before_welcome = s.st == session_t::st_e::hello_sent;
          attempt_hook(before_welcome ? "disconnect" : "pipe-broken", e);
          teardown("pipe-broken", -1);
          return {k_use_backoff, false, false, false};
        }
        vripc_msg_hdr_t h {};
        const char *why = "";
        if (!proto::check_header(s.rbuf, n, h, why)) {
          return protocol_error(why);
        }
        if (!proto::check_body_size(h, n, why)) {
          return protocol_error(why);
        }
        if (h.seq <= s.last_seq_in) {
          return protocol_error("seq");
        }
        s.last_seq_in = h.seq;

        if (h.type == VRIPC_MSG_REJECT) {
          vripc_reject_t rj {};
          std::memcpy(&rj, s.rbuf, sizeof(rj));
          {
            std::lock_guard<std::mutex> lk(stats_mtx_);
            ++stats_.rejects_received;
            stats_.last_reject_reason = rj.reason;
            stats_.last_reject_detail = rj.detail;
          }
          VRDRV_LOG_WARN("ipc rejected reason=%s code=%u detail=%u", reject_name(rj.reason), rj.reason, rj.detail);
          attempt_hook("reject", rj.reason);
          if (cb_.on_rejected) {
            cb_.on_rejected(rj.reason, rj.detail);
          }
          const bool abi = rj.reason == VRIPC_REJ_ABI_MISMATCH && s.st == session_t::st_e::hello_sent;
          signal_first(abi ? first_e::dead : first_e::rejected);
          teardown(abi ? "rejected-abi" : "rejected", -1);
          if (abi) {
            VRDRV_LOG_ERROR("ipc dead reason=abi-mismatch own-abi=%u server-abi=%u", (unsigned) VRIPC_ABI_VERSION, rj.detail);
            return {0, true, false, false};
          }
          return {k_use_backoff, false, false, false};
        }

        if (s.st == session_t::st_e::hello_sent) {
          if (h.type != VRIPC_MSG_WELCOME) {
            return protocol_error("expected-welcome");
          }
          // ── D4：WELCOME → map → 驗 header／config → MAPPED ──
          vripc_welcome_t wl {};
          std::memcpy(&wl, s.rbuf, sizeof(wl));
          if (!proto::check_welcome(wl, qpf_, why)) {
            close_if_plausible(wl.dup_shm_handle);
            close_if_plausible(wl.dup_evt_trk);
            close_if_plausible(wl.dup_evt_frm);
            return protocol_error(why);
          }
#if defined(VRDRV_PEER_HOOKS)
          if (cfg_.peer_mode && cfg_.hooks.on_welcome) {
            cfg_.hooks.on_welcome(wl);
          }
          if (cfg_.peer_mode && cfg_.hooks.escalate) {
            // U30：不具名物件是否套用 server 傳入的 SD（D:P(A;;GA;;;SY)）。能升權代表沒套用；只記錄結果。
            struct esc_t {
              const char *name;
              uint64_t v;
              DWORD access;
            } esc[3] = {
              {"shm", wl.dup_shm_handle, SECTION_ALL_ACCESS},
              {"evt-trk", wl.dup_evt_trk, EVENT_ALL_ACCESS},
              {"evt-frm", wl.dup_evt_frm, EVENT_ALL_ACCESS},
            };
            for (const auto &e : esc) {
              HANDLE dup = nullptr;
              const BOOL granted = DuplicateHandle(GetCurrentProcess(), to_handle(e.v), GetCurrentProcess(), &dup, e.access, FALSE, 0);
              const DWORD ge = granted ? 0 : GetLastError();
              if (dup) {
                CloseHandle(dup);
              }
              VRDRV_LOG_INFO("peer escalate obj=%s result=%s err=%lu", e.name, granted ? "granted" : "denied", ge);
            }
          }
#endif
          void *view = MapViewOfFile(to_handle(wl.dup_shm_handle), FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
          const DWORD map_err = view ? 0 : GetLastError();
          CloseHandle(to_handle(wl.dup_shm_handle));  // view 保住 section
          generation_ptr g(new generation_t());
          g->welcome_ = wl;
          g->evt_trk_ = to_handle(wl.dup_evt_trk);
          g->evt_frm_ = to_handle(wl.dup_evt_frm);
          if (!view) {
            VRDRV_LOG_WARN("ipc map-failed err=%lu", map_err);
            return protocol_error("map");
          }
          g->shm_ = static_cast<vripc_shm_t *>(view);
          MEMORY_BASIC_INFORMATION mbi {};
          if (VirtualQuery(view, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize < sizeof(vripc_shm_t)) {
            return protocol_error("shm-region-size");
          }
          vripc_shm_header_t hdr {};
          g->read_header(hdr);
          if (!proto::check_shm_header(hdr, wl, why)) {
            return protocol_error(why);
          }
          VRIPC_COMPILER_BARRIER();
          std::memcpy(&g->config_, (const void *) &g->shm_->config, sizeof(g->config_));
          VRIPC_COMPILER_BARRIER();
          if (!proto::check_config(g->config_, why)) {
            return protocol_error(why);
          }
          s.gen = g;
          s.st = session_t::st_e::mapped;
          s.link_state = VRIPC_DRV_MAPPED;
          s.mapped_qpc = qpc_now();
          s.next_hb_qpc = s.mapped_qpc + ms_to_qpc(qpf_, proto::k_heartbeat_ms);
          write_status_block(g, s.link_state);
          {
            std::lock_guard<std::mutex> lk(gen_mtx_);
            gen_ = g;
          }
          {
            std::lock_guard<std::mutex> lk(stats_mtx_);
            ++stats_.handshakes;
            stats_.last_generation = g->id();
          }
          const auto &c = g->config_;
          VRDRV_LOG_INFO("ipc handshake gen=%llu server=%u.%u.%u abi=%u idle=%d ring=%ux%u hz=%u.%03u flags=0x%x armed=%u",
                         (unsigned long long) wl.generation, wl.server_version_packed >> 24, (wl.server_version_packed >> 16) & 0xFFu,
                         wl.server_version_packed & 0xFFFFu, wl.abi_version, g->idle() ? 1 : 0, c.packed_width, c.packed_height,
                         c.refresh_mhz / 1000, c.refresh_mhz % 1000, wl.welcome_flags, hdr.armed);
          attempt_hook("welcome", 0);
          signal_first(first_e::mapped);
          if (cb_.on_mapped) {
            cb_.on_mapped(g, hdr.armed != 0);
          }
          s.deadline_qpc = g->idle() ? 0 : qpc_now() + ms_to_qpc(qpf_, proto::k_textures_timeout_ms);
#if defined(VRDRV_PEER_HOOKS)
          if (cfg_.peer_mode && cfg_.hooks.close_after_welcome) {
            // sec-M1：關掉收到的 event，讓 peer 以新物件佔用同一個值；之後 server 若從遠端關這些值就會打到 peer 的新物件。
            uint64_t closed[2] = {wl.dup_evt_trk, wl.dup_evt_frm};
            CloseHandle(g->evt_trk_);
            CloseHandle(g->evt_frm_);
            g->evt_trk_ = nullptr;
            g->evt_frm_ = nullptr;
            VRDRV_LOG_INFO("peer close-after-welcome closed=2");
            if (cfg_.hooks.on_handles_closed) {
              cfg_.hooks.on_handles_closed(2, closed);
            }
          }
          if (cfg_.peer_mode && cfg_.hooks.never_read_after_mapped) {
            s.never_read = true;
            VRDRV_LOG_INFO("peer never-read active");
            continue;  // 不再 post read
          }
#endif
        } else {
          // ── MAPPED／OPERATIONAL ──
          switch (h.type) {
            case VRIPC_MSG_TEXTURES:
              {
                vripc_textures_t t {};
                std::memcpy(&t, s.rbuf, sizeof(t));
                uint64_t hv[VRIPC_TEX_COUNT + 2] = {t.tex_handle[0], t.tex_handle[1], t.tex_handle[2], t.shared_fence_handle, t.consumed_fence_handle};
                auto close_all = [&]() {
                  for (const uint64_t v : hv) {
                    close_if_plausible(v);
                  }
                };
                if (s.st != session_t::st_e::mapped || s.textures_done) {
                  close_all();
                  return protocol_error("textures-unexpected");
                }
                uint32_t rej = proto::check_textures(t, s.gen->id(), s.gen->config(), why);
                uint32_t detail = 0;
                if (rej == 0) {
                  if (!cb_.on_textures) {
                    rej = VRIPC_REJ_TEXTURE_INVALID;
                    why = "no-handler";
                  } else {
                    const HANDLE tex[VRIPC_TEX_COUNT] = {to_handle(hv[0]), to_handle(hv[1]), to_handle(hv[2])};
                    const textures_result_t r = cb_.on_textures(s.gen, t, tex, to_handle(hv[3]), to_handle(hv[4]));
                    if (!r.ok) {
                      rej = r.reject_reason != 0 ? r.reject_reason : VRIPC_REJ_TEXTURE_INVALID;
                      detail = r.reject_detail;
                      why = "open-failed";
                    }
                  }
                }
                close_all();  // 開好的物件由 COM 參照保住；handle 本身一律由這裡關（所有權已移交給本行程）
                s.textures_done = true;
                if (rej != 0) {
                  VRDRV_LOG_WARN("ipc textures gen=%llu result=reject reason=%s why=%s detail=0x%08x", (unsigned long long) s.gen->id(), reject_name(rej), why, detail);
                  send_reject(rej, detail);
                  teardown("textures-rejected", -1);
                  return {k_use_backoff, false, false, false};
                }
                vripc_ready_t rd {};
                rd.hdr = proto::make_header(VRIPC_MSG_READY, (uint32_t) sizeof(rd), 0);
                rd.generation = s.gen->id();
                const auto wr = s.write_raw(&rd, (uint32_t) sizeof(rd));
                if (wr != session_t::wr_e::ok) {
                  teardown(wr == session_t::wr_e::timeout ? "write-timeout" : "pipe-broken", -1);
                  return {k_use_backoff, false, false, false};
                }
                s.st = session_t::st_e::operational;
                s.link_state = VRIPC_DRV_READY;
                s.deadline_qpc = 0;
                write_status_block(s.gen, s.link_state);
                {
                  std::lock_guard<std::mutex> lk(stats_mtx_);
                  ++stats_.operational;
                }
                VRDRV_LOG_INFO("ipc textures gen=%llu ring=%ux%u result=ok", (unsigned long long) s.gen->id(), t.width, t.height);
                break;
              }
            case VRIPC_MSG_STATE:
              {
                vripc_state_t m {};
                std::memcpy(&m, s.rbuf, sizeof(m));
                if (m.generation != s.gen->id()) {
                  VRDRV_LOG_WARN("ipc state-ignored reason=generation kind=0x%x", m.kind);
                  break;
                }
                switch (m.kind) {
                  case VRIPC_ST_ARM:
                  case VRIPC_ST_DISARM:
                  case VRIPC_ST_REQUEST_QUIT:
                    if (cb_.on_server_state) {
                      cb_.on_server_state(m.kind, m.arg);
                    }
                    break;
                  case VRIPC_ST_DEV_SET_V2P:
                    if (!s.gen->dev_pacing()) {
                      VRDRV_LOG_WARN("ipc state-ignored reason=dev-pacing-off kind=0x%x", m.kind);
                    } else if (cb_.on_server_state) {
                      cb_.on_server_state(m.kind, m.arg);
                    }
                    break;
                  default:
                    VRDRV_LOG_WARN("ipc state-ignored reason=unknown kind=0x%x", m.kind);
                    break;
                }
                break;
              }
            case VRIPC_MSG_BYE:
              {
                vripc_bye_t b {};
                std::memcpy(&b, s.rbuf, sizeof(b));
                VRDRV_LOG_INFO("ipc bye reason=%s detail=%u", bye_name(b.reason), b.detail);
                switch (b.reason) {
                  case VRIPC_BYE_RECONFIG:
                    teardown("bye-reconfig", -1);
                    return {0, false, false, true};  // 立刻重連（不退避），新 generation
                  case VRIPC_BYE_DEVICE_LOST:
                    teardown("bye-device-lost", -1);
                    return {0, false, false, true};
                  case VRIPC_BYE_SERVER_SHUTDOWN:
                    teardown("bye-server-shutdown", -1);
                    return {k_use_backoff, false, false, true};
                  case VRIPC_BYE_PROTOCOL_ERROR:
                    teardown("bye-protocol-error", -1);
                    return {k_use_backoff, false, false, true};
                  default:
                    teardown("bye", -1);
                    return {k_use_backoff, false, false, true};
                }
              }
            default:
              return protocol_error("unexpected-type");
          }
        }
        if (!s.post_read()) {
          teardown("pipe-broken", -1);
          return {k_use_backoff, false, false, false};
        }
      }

      // ── 計時器 ──
      const int64_t t = qpc_now();
      if (s.deadline_qpc != 0 && t >= s.deadline_qpc) {
        if (s.st == session_t::st_e::hello_sent) {
          attempt_hook("timeout", 0);
          teardown("hello-timeout", -1);
          return {k_use_backoff, false, false, false};
        }
        // MAPPED（有 config）等不到 TEXTURES
        send_reject(VRIPC_REJ_TIMEOUT, 0);
        teardown("textures-timeout", -1);
        return {k_use_backoff, false, false, false};
      }
      if (s.gen && t >= s.next_hb_qpc) {
        write_status_block(s.gen, s.link_state);
        s.next_hb_qpc = t + ms_to_qpc(qpf_, proto::k_heartbeat_ms);
        // server heartbeat（§B.6 OPERATIONAL：> 1 s 只進 standby、不斷線）
        vripc_shm_header_t hdr {};
        s.gen->read_header(hdr);
        if (hdr.magic != VRIPC_MAGIC_SHM || hdr.generation != s.gen->id()) {
          if (!s.header_bad_logged) {
            s.header_bad_logged = true;
            return protocol_error("shm-header-changed");
          }
        }
        const int64_t ref = hdr.server_heartbeat_qpc > s.mapped_qpc ? hdr.server_heartbeat_qpc : s.mapped_qpc;
        const bool stale = (t - ref) > ms_to_qpc(qpf_, proto::k_server_stale_ms);
        if (stale != s.server_stale) {
          s.server_stale = stale;
          VRDRV_LOG_WARN("ipc server-heartbeat %s gen=%llu", stale ? "stale" : "recovered", (unsigned long long) s.gen->id());
          if (cb_.on_server_heartbeat) {
            cb_.on_server_heartbeat(stale);
          }
        }
      }
    }
  }

}  // namespace vrdrv
