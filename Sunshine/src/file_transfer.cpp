/**
 * @file src/file_transfer.cpp
 * @brief In-stream 雙向檔案傳輸 manager 實作。對應 plan VipleStream §N。
 */

#include "file_transfer.h"

#include "logging.h"
#include "utility.h"  // 必須先於 uuid.h — uuid_t::string() 用了 util::hex()

#include <algorithm>
#include <cstdlib>

// Windows 平台 RPC headers（被 shlobj.h 拉進來）會 `#define uuid_t UUID`，
// 把後續對 `uuid_util::uuid_t` 的 namespace-qualified reference 也展開成
// `uuid_util::UUID`，造成 "'uuid_util::UUID' has not been declared" 編譯錯
// 誤。必須在 Windows headers 之後 `#undef uuid_t`，再 include 自家 uuid.h。
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <shlobj.h>
  #include <windows.h>
  #include <wtsapi32.h>
  #ifdef uuid_t
    #undef uuid_t
  #endif
#endif

#include "uuid.h"

namespace file_transfer {

  namespace {

    constexpr std::chrono::milliseconds k_default_poll_timeout {30000};

    /**
     * 把任意 std::string filename 過濾成「只能是單一檔名」。
     * 拒：ASCII 控制字元、`/`、`\`、`:`、`*`、`?`、`"`、`<`、`>`、`|`、開頭 `.`、`..`、空字串、長度 > 255。
     */
    std::string sanitize_impl(const std::string &raw) {
      if (raw.empty() || raw.size() > 255) {
        return {};
      }
      // 拒絕純點檔名（`.` / `..`），這兩個是 path traversal 經典 marker
      if (raw == "." || raw == "..") {
        return {};
      }
      static constexpr std::string_view forbidden = "/\\:*?\"<>|";
      std::string out;
      out.reserve(raw.size());
      for (unsigned char ch : raw) {
        if (ch < 0x20) {
          return {};  // 任意控制字元包含 NUL 直接拒
        }
        if (forbidden.find(static_cast<char>(ch)) != std::string_view::npos) {
          return {};
        }
        out.push_back(static_cast<char>(ch));
      }
      return out;
    }

    /**
     * 在目標目錄找一個尚未存在的檔名（衝突自動加 `-1`、`-2` suffix）。
     */
    std::filesystem::path resolve_collision(const std::filesystem::path &dir,
                                            const std::string &filename) {
      std::filesystem::path target = dir / filename;
      if (!std::filesystem::exists(target)) {
        return target;
      }
      std::filesystem::path stem = target.stem();
      std::filesystem::path ext = target.extension();
      for (int i = 1; i < 1000; ++i) {
        std::filesystem::path candidate =
            dir / (stem.string() + "-" + std::to_string(i) + ext.string());
        if (!std::filesystem::exists(candidate)) {
          return candidate;
        }
      }
      // Pathological：1000 個都撞名，加 UUID 後綴保證唯一
      return dir / (stem.string() + "-" + uuid_util::uuid_t::generate().string() + ext.string());
    }

  }  // namespace

  manager &manager::instance() {
    static manager s;
    return s;
  }

  std::string manager::make_token() {
    return uuid_util::uuid_t::generate().string();
  }

  std::string manager::sanitize_filename(const std::string &raw) {
    return sanitize_impl(raw);
  }

  std::filesystem::path manager::downloads_dir() {
#ifdef _WIN32
    // Sunshine 以 LocalSystem service 跑，process token 是 SYSTEM。
    // 直接 SHGetKnownFolderPath(nullptr) 會回 `C:\Windows\system32\config\
    // systemprofile\Downloads` — 互動用戶看不到。先抓 active console session
    // 的 user token，用它的 context 查 Downloads（user-visible 路徑）。
    // 失敗時 fallback 到 SYSTEM profile（總比寫不出來好）。
    auto try_active_user = [](std::filesystem::path &out) -> bool {
      DWORD session_id = WTSGetActiveConsoleSessionId();
      if (session_id == 0xFFFFFFFF) return false;
      HANDLE user_tok = nullptr;
      if (!WTSQueryUserToken(session_id, &user_tok)) {
        BOOST_LOG(warning) << "[VIPLE-XFER] WTSQueryUserToken failed (need SYSTEM "
                              "or sufficient priv): err=" << GetLastError();
        return false;
      }
      PWSTR path_w = nullptr;
      bool ok = false;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, user_tok, &path_w))) {
        out = std::filesystem::path(std::wstring(path_w));
        CoTaskMemFree(path_w);
        ok = true;
      }
      CloseHandle(user_tok);
      return ok;
    };

    std::filesystem::path p;
    if (try_active_user(p)) {
      return p;
    }
    PWSTR path_w = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, KF_FLAG_CREATE, nullptr, &path_w))) {
      std::wstring ws = path_w;
      CoTaskMemFree(path_w);
      return std::filesystem::path(ws);
    }
    // 最終 fallback：%USERPROFILE%\Downloads / 寫 C:\Downloads
    if (const char *up = std::getenv("USERPROFILE")) {
      return std::filesystem::path(up) / "Downloads";
    }
    return std::filesystem::path("C:/Downloads");
#else
    // Linux：XDG_DOWNLOAD_DIR 不一定存在，fallback HOME/Downloads
    if (const char *xdg = std::getenv("XDG_DOWNLOAD_DIR")) {
      if (*xdg) {
        return std::filesystem::path(xdg);
      }
    }
    if (const char *home = std::getenv("HOME")) {
      return std::filesystem::path(home) / "Downloads";
    }
    return std::filesystem::path("/tmp");
#endif
  }

  namespace {
    void init_cmd(command &cmd) {
      cmd.size = 0;
      cmd.is_directory = false;
    }
  }

  std::string manager::queue_send_to_client(const std::string &client_uuid,
                                            const std::filesystem::path &local_path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(local_path, ec);
    if (ec) {
      BOOST_LOG(warning) << "[VIPLE-XFER] queue_send_to_client: file_size failed for "
                         << local_path.string() << ": " << ec.message();
      return {};
    }

    auto token = make_token();
    auto t = std::make_shared<transfer>();
    t->token = token;
    t->dir = direction::to_client;
    t->st = state::pending;
    t->local_path = local_path;
    t->display_name = local_path.filename().string();
    t->total_bytes = size;
    t->bytes_done = 0;

    {
      std::lock_guard lk {m_mutex};
      // §N.5.bug — 防衛性 sweep：避免上次 stale pending 沒被 busy() 路徑
      // 觸發過 sweep (例如 tray callback 沒先 check busy() 直接走 queue)。
      sweep_stale_locked();
      m_active.emplace(token, t);
      command cmd;
      init_cmd(cmd);
      cmd.type = command_type::download_to_client;
      cmd.token = token;
      cmd.filename = t->display_name;
      cmd.size = size;
      m_queues[client_uuid].push(std::move(cmd));
    }
    m_cv.notify_all();

    BOOST_LOG(info) << "[VIPLE-XFER] queued send-to-client token=" << token
                    << " file=" << local_path.string() << " size=" << size;
    return token;
  }

  std::string manager::queue_receive_from_client(const std::string &client_uuid,
                                                 const std::string &path,
                                                 const std::string &client_filename,
                                                 bool is_directory,
                                                 std::uint64_t expected_size) {
    auto sanitized = sanitize_filename(client_filename);
    if (sanitized.empty()) {
      BOOST_LOG(warning) << "[VIPLE-XFER] queue_receive_from_client: rejected filename '"
                         << client_filename << "'";
      return {};
    }
    // 資料夾的話 server 端落地檔自動加 `.zip`（client 會 zip 後上傳）
    std::string target_name = is_directory ? (sanitized + ".zip") : sanitized;

    auto dir = downloads_dir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto target = resolve_collision(dir, target_name);

    auto token = make_token();
    auto t = std::make_shared<transfer>();
    t->token = token;
    t->dir = direction::from_client;
    t->st = state::pending;
    t->local_path = target;
    t->display_name = target.filename().string();
    t->total_bytes = expected_size;
    t->bytes_done = 0;

    {
      std::lock_guard lk {m_mutex};
      // §N.5.bug — 同 queue_send_to_client，防衛性 sweep stale pending。
      sweep_stale_locked();
      m_active.emplace(token, t);
      command cmd;
      init_cmd(cmd);
      cmd.type = command_type::upload_from_client;
      cmd.token = token;
      cmd.filename = sanitized;
      cmd.size = expected_size;
      cmd.path = path;
      cmd.is_directory = is_directory;
      m_queues[client_uuid].push(std::move(cmd));
    }
    m_cv.notify_all();

    BOOST_LOG(info) << "[VIPLE-XFER] queued receive-from-client token=" << token
                    << " src_path=" << path << " src_name=" << sanitized
                    << " is_dir=" << is_directory
                    << " target=" << target.string()
                    << " expected_size=" << expected_size;
    return token;
  }

  std::string manager::queue_list_dir(const std::string &client_uuid,
                                      const std::string &path) {
    auto query_id = make_token();
    {
      std::lock_guard lk {m_mutex};
      command cmd;
      init_cmd(cmd);
      cmd.type = command_type::list_dir;
      cmd.token = query_id;
      cmd.path = path;
      m_queues[client_uuid].push(std::move(cmd));
      // Clear stale listing — UI 將等下一份新的
      m_last_listing.erase(client_uuid);
    }
    m_cv.notify_all();
    BOOST_LOG(info) << "[VIPLE-XFER] requested list_dir from client=" << client_uuid
                    << " query_id=" << query_id << " path='" << path << "'";
    return query_id;
  }

  void manager::cancel(const std::string &client_uuid, const std::string &token) {
    {
      std::lock_guard lk {m_mutex};
      auto it = m_active.find(token);
      if (it != m_active.end()) {
        it->second->cancel_flag.store(true);
        it->second->st = state::canceled;
        BOOST_LOG(info) << "[VIPLE-XFER] cancel flagged token=" << token;
      }
      command cmd;
      init_cmd(cmd);
      cmd.type = command_type::cancel;
      cmd.token = token;
      m_queues[client_uuid].push(std::move(cmd));
    }
    m_cv.notify_all();
  }

  std::optional<command> manager::poll(const std::string &client_uuid,
                                       std::chrono::milliseconds timeout) {
    // timeout == 0 ⇒ 真正的 non-blocking check（給 nvhttp poll endpoint 用，
    // 每 2s 由 client 重撥；無 cmd 立即回 204）。負值才用 default。
    if (timeout.count() < 0) {
      timeout = k_default_poll_timeout;
    }

    std::unique_lock lk {m_mutex};
    auto &q = m_queues[client_uuid];
    if (timeout.count() == 0) {
      if (q.empty()) {
        return std::nullopt;
      }
    } else {
      if (!m_cv.wait_for(lk, timeout, [&] { return !q.empty(); })) {
        return std::nullopt;
      }
    }
    auto cmd = q.front();
    q.pop();
    return cmd;
  }

  std::optional<transfer_snapshot> manager::get_transfer(const std::string &token) {
    std::lock_guard lk {m_mutex};
    auto it = m_active.find(token);
    if (it == m_active.end()) {
      return std::nullopt;
    }
    transfer_snapshot snap;
    snap.token = it->second->token;
    snap.dir = it->second->dir;
    snap.st = it->second->st;
    snap.local_path = it->second->local_path;
    snap.display_name = it->second->display_name;
    snap.total_bytes = it->second->total_bytes;
    snap.bytes_done = it->second->bytes_done;
    snap.error_msg = it->second->error_msg;
    snap.cancel_flag = it->second->cancel_flag.load();
    return snap;
  }

  void manager::store_listing(const std::string &client_uuid, std::string json_listing) {
    std::lock_guard lk {m_mutex};
    m_last_listing[client_uuid] = std::move(json_listing);
    BOOST_LOG(debug) << "[VIPLE-XFER] stored listing for client=" << client_uuid
                     << " bytes=" << m_last_listing[client_uuid].size();
  }

  std::string manager::take_listing(const std::string &client_uuid) {
    std::lock_guard lk {m_mutex};
    auto it = m_last_listing.find(client_uuid);
    if (it == m_last_listing.end()) {
      return {};
    }
    return it->second;
  }

  bool manager::open_for_send(const std::string &token, std::ifstream &out_stream) {
    std::shared_ptr<transfer> t;
    {
      std::lock_guard lk {m_mutex};
      auto it = m_active.find(token);
      if (it == m_active.end()) {
        return false;
      }
      t = it->second;
    }
    if (t->dir != direction::to_client) {
      return false;
    }
    out_stream.open(t->local_path, std::ios::binary);
    if (!out_stream.is_open()) {
      BOOST_LOG(warning) << "[VIPLE-XFER] open_for_send failed: " << t->local_path.string();
      finalize(token, false, "open failed");
      return false;
    }
    {
      std::lock_guard lk {m_mutex};
      t->st = state::running;
    }
    return true;
  }

  bool manager::open_for_receive(const std::string &token, std::ofstream &out_stream) {
    std::shared_ptr<transfer> t;
    {
      std::lock_guard lk {m_mutex};
      auto it = m_active.find(token);
      if (it == m_active.end()) {
        return false;
      }
      t = it->second;
    }
    if (t->dir != direction::from_client) {
      return false;
    }
    out_stream.open(t->local_path, std::ios::binary | std::ios::trunc);
    if (!out_stream.is_open()) {
      BOOST_LOG(warning) << "[VIPLE-XFER] open_for_receive failed: " << t->local_path.string();
      finalize(token, false, "open failed");
      return false;
    }
    {
      std::lock_guard lk {m_mutex};
      t->st = state::running;
    }
    return true;
  }

  void manager::report_progress(const std::string &token, std::uint64_t delta_bytes) {
    std::lock_guard lk {m_mutex};
    auto it = m_active.find(token);
    if (it == m_active.end()) {
      return;
    }
    it->second->bytes_done += delta_bytes;
  }

  void manager::finalize(const std::string &token, bool success, std::string error_msg) {
    std::shared_ptr<transfer> t;
    {
      std::lock_guard lk {m_mutex};
      auto it = m_active.find(token);
      if (it == m_active.end()) {
        return;
      }
      t = it->second;
      t->st = success ? state::done : state::failed;
      t->error_msg = std::move(error_msg);
    }
    BOOST_LOG(info) << "[VIPLE-XFER] finalize token=" << token
                    << " success=" << success
                    << " bytes=" << t->bytes_done << "/" << t->total_bytes
                    << (t->error_msg.empty() ? "" : (" err=" + t->error_msg));
  }

  bool manager::busy() const {
    std::lock_guard lk {m_mutex};
    sweep_stale_locked();
    for (const auto &[tok, t] : m_active) {
      if (t->st == state::pending || t->st == state::running) {
        return true;
      }
    }
    return false;
  }

  // §N.5.bug (2026-05-13) — 見 file_transfer.h decl 註解。pending 超過
  // STALE_PENDING_TIMEOUT 仍沒被 `open_for_send` / `open_for_receive` 切到
  // running，視為 client side 沒抵達 server handler (SSL fail / app crash /
  // network drop)，標 failed 讓 `busy()` 釋出鎖，使用者下個 send 不被擋.
  void manager::sweep_stale_locked() const {
    const auto now = std::chrono::steady_clock::now();
    for (auto &[tok, t] : m_active) {
      if (t->st != state::pending) continue;
      const auto age = now - t->started_at;
      if (age <= STALE_PENDING_TIMEOUT) continue;
      const auto age_s = std::chrono::duration_cast<std::chrono::seconds>(age).count();
      BOOST_LOG(warning) << "[VIPLE-XFER] sweep_stale token=" << tok
                         << " age=" << age_s << "s pending→failed"
                         << " (client never reached /transfer/blob handler;"
                         << " typical cause: SSL fail / app crash / network drop)";
      t->st = state::failed;
      t->error_msg = "stale_pending_timeout";
    }
  }

  void manager::abort_all(const std::string &reason) {
    std::lock_guard lk {m_mutex};
    for (auto &[tok, t] : m_active) {
      if (t->st == state::pending || t->st == state::running) {
        t->cancel_flag.store(true);
        t->st = state::canceled;
        t->error_msg = reason;
        BOOST_LOG(info) << "[VIPLE-XFER] abort_all token=" << tok << " reason=" << reason;
      }
    }
    m_queues.clear();
    m_last_listing.clear();
  }

}  // namespace file_transfer
