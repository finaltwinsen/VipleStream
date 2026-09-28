/**
 * @file src/platform/windows/vr_admin_pipe.cpp
 * @brief VipleStream §VR（M1b S1-18、§F.5）：admin pipe（server 端）與 CLI（client 端）；設計見 vr_admin_pipe.h。
 */
// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// platform includes
#include <Windows.h>
#include <aclapi.h>
#include <sddl.h>

// local includes
#include "misc.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/vr/vr_bridge.h"
#include "src/vr/vr_selftest.h"
#include "vr_admin_pipe.h"

using namespace std::literals;

namespace platf::vr_admin {
  namespace {
    constexpr const wchar_t *k_pipe_name = L"\\\\.\\pipe\\VipleStreamAdmin";
    // §F.5：owner/group = SYSTEM；protected DACL 只有 SYSTEM 與 BUILTIN\Administrators（0x12019b＝不含
    // FILE_CREATE_PIPE_INSTANCE 的讀寫）；SACL 的 mandatory label 是 High＋NW/NR：非提升（medium）的行程即使
    // token 裡有 BA（UAC 的 deny-only）也打不開。
    constexpr const wchar_t *k_pipe_sddl = L"O:SYG:SYD:P(A;;GA;;;SY)(A;;0x12019b;;;BA)S:(ML;;NWNR;;;HI)";
    constexpr DWORD k_max_msg = 4096;  ///< 一則訊息的上限（§F.5）
    constexpr DWORD k_pipe_out_buffer = 16384;  ///< 建議值；大於單則上限，讓連續幾行不必一行一行等 CLI 讀
    constexpr DWORD k_first_msg_timeout_ms = 5000;
    constexpr DWORD k_write_timeout_ms = 10000;  ///< CLI 太久沒讀（被暫停等）就當成斷線
    constexpr auto k_retry_interval = 30s;  ///< 建立失敗（被搶名）後的重試間隔（sec-m13）
    constexpr std::size_t k_history_max = 4000;  ///< vr-status 保留的行數上限
    constexpr auto k_shutdown_wait = 15s;  ///< server 關閉時等 selftest 清理的上限

    // ── 小工具 ──────────────────────────────────────────────────────────

    struct handle_t {
      HANDLE h = nullptr;

      handle_t() = default;

      explicit handle_t(HANDLE v):
          h {v == INVALID_HANDLE_VALUE ? nullptr : v} {
      }

      handle_t(const handle_t &) = delete;
      handle_t &operator=(const handle_t &) = delete;

      ~handle_t() {
        reset();
      }

      void reset(HANDLE v = nullptr) {
        if (h) {
          CloseHandle(h);
        }
        h = v == INVALID_HANDLE_VALUE ? nullptr : v;
      }

      explicit operator bool() const {
        return h != nullptr;
      }
    };

    /// 手動重設 event 的 OVERLAPPED
    struct ovl_t {
      OVERLAPPED o {};
      handle_t evt {CreateEventW(nullptr, TRUE, FALSE, nullptr)};

      ovl_t() {
        o.hEvent = evt.h;
      }

      void reset() {
        ResetEvent(evt.h);
        o = {};
        o.hEvent = evt.h;
      }
    };

    std::string utc_run_id() {
      SYSTEMTIME st {};
      GetSystemTime(&st);
      return std::format("{:04}{:02}{:02}T{:02}{:02}{:02}Z", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }

    std::string dump_json(const nlohmann::json &j) {
      return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    }

    // ── selftest 結果的持久化（§F.5、ops-m17）─────────────────────────────
    //
    // `<install>\config\steamvr\selftest\<run>\summary.json`：開跑時寫 state=running，跑完（含 abort）覆寫成
    // state=done＋全部輸出行。service 重啟（部署、自我更新、當掉後重生）之後，`--vr-status` 在記憶體裡沒有紀錄時
    // 讀最近一份；state 仍是 running 的代表前一個 server 在執行中結束（回報 interrupted）。
    // 只有 SYSTEM 寫；目錄與檔案的 SD 與 §D.11 第 2 點相同（V5 的 secure_fs 建同一組目錄時會驗到一樣的 SD 而沿用）。
    // 內容沒有機密：輸出行本來就遵守 selftest 的 log 衛生（不含 handle 值、完整 SID／GUID）。
    namespace summary_store {
      /// §D.11 第 2 點：protected、OICI；BU 只有讀＋執行（0x1200a9）
      constexpr const wchar_t *k_dir_sddl = L"O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
      constexpr const wchar_t *k_file_sddl = L"O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;BU)";
      constexpr std::uint64_t k_max_bytes = 8ull * 1024 * 1024;  ///< 4000 行 × 每行最多約 1 KiB 的上限，再留餘裕
      constexpr std::size_t k_max_line = 8192;
      /// §D.11 第 1 點：SY／BA 以外的主體不得有的權限
      constexpr DWORD k_write_rights = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_DELETE_CHILD | DELETE | WRITE_DAC |
                                       WRITE_OWNER | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;

      class local_sd_t {
      public:
        explicit local_sd_t(const wchar_t *sddl) {
          if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &p_, nullptr)) {
            p_ = nullptr;
          }
        }

        ~local_sd_t() {
          if (p_) {
            LocalFree(p_);
          }
        }

        local_sd_t(const local_sd_t &) = delete;
        local_sd_t &operator=(const local_sd_t &) = delete;

        PSECURITY_DESCRIPTOR get() const {
          return p_;
        }

      private:
        PSECURITY_DESCRIPTOR p_ = nullptr;
      };

      /// run id 是 utc_run_id() 的 `YYYYMMDDTHHMMSSZ`；其他名稱（例：之後 T0-secfs 的 secfs-<utc>）一律不碰
      bool valid_run_id(std::wstring_view s) {
        if (s.size() != 16 || s[8] != L'T' || s[15] != L'Z') {
          return false;
        }
        for (std::size_t i = 0; i < 15; ++i) {
          if (i != 8 && (s[i] < L'0' || s[i] > L'9')) {
            return false;
          }
        }
        return true;
      }

      /**
       * @brief 已存在的目錄：不是 reparse point、真的是目錄；owned 時另要求 owner = SYSTEM、protected DACL、
       *        SY／BA 以外沒有寫入類權限（§D.11 第 1 點的子集；完整的逐層檢查在 V5 的 secure_fs）。
       * @return 空字串＝通過；否則是原因（英文短字，進 log）。
       */
      std::string check_dir(const std::wstring &path, bool owned) {
        handle_t h {CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!h) {
          return std::format("open-{}", GetLastError());
        }
        FILE_ATTRIBUTE_TAG_INFO tag {};
        if (!GetFileInformationByHandleEx(h.h, FileAttributeTagInfo, &tag, sizeof(tag))) {
          return std::format("attr-{}", GetLastError());
        }
        if (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
          return "reparse";
        }
        if (!(tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
          return "not-dir";
        }
        if (!owned) {
          return {};
        }
        PSID owner = nullptr;
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR sd = nullptr;
        const DWORD err = GetSecurityInfo(h.h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &dacl, nullptr, &sd);
        if (err != ERROR_SUCCESS) {
          return std::format("sd-{}", err);
        }
        std::string why;
        SECURITY_DESCRIPTOR_CONTROL ctl = 0;
        DWORD rev = 0;
        if (!owner || !IsWellKnownSid(owner, WinLocalSystemSid)) {
          why = "owner";
        } else if (!dacl || !GetSecurityDescriptorControl(sd, &ctl, &rev) || !(ctl & SE_DACL_PROTECTED)) {
          why = "dacl";
        } else {
          for (DWORD i = 0; i < dacl->AceCount && why.empty(); ++i) {
            void *p = nullptr;
            if (!GetAce(dacl, i, &p) || static_cast<const ACE_HEADER *>(p)->AceType != ACCESS_ALLOWED_ACE_TYPE) {
              continue;  // deny ACE 只會更嚴
            }
            const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(p);
            PSID sid = const_cast<DWORD *>(&ace->SidStart);
            const bool admin = IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid);
            if (!admin && (ace->Mask & k_write_rights)) {
              why = "dacl";
            }
          }
        }
        LocalFree(sd);
        return why;
      }

      /// 建目錄（§D.11 的 SD）；已存在就驗（不改名、不修：那是 V5 secure_fs 的工作，這裡只放棄持久化）
      std::string ensure_dir(const std::wstring &path) {
        local_sd_t sd {k_dir_sddl};
        if (!sd.get()) {
          return std::format("sddl-{}", GetLastError());
        }
        SECURITY_ATTRIBUTES sa {sizeof(sa), sd.get(), FALSE};
        if (CreateDirectoryW(path.c_str(), &sa)) {
          return {};
        }
        const DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
          return std::format("create-{}", err);
        }
        return check_dir(path, true);
      }

      /// 先寫 summary.json.tmp（CREATE_NEW、不跟隨 reparse point、明確的 SD）再 rename 蓋過正式檔
      std::string write_file(const std::wstring &dir, const std::string &content) {
        local_sd_t sd {k_file_sddl};
        if (!sd.get()) {
          return std::format("sddl-{}", GetLastError());
        }
        SECURITY_ATTRIBUTES sa {sizeof(sa), sd.get(), FALSE};
        const std::wstring final_path = dir + L"\\summary.json";
        const std::wstring temp_path = final_path + L".tmp";
        DeleteFileW(temp_path.c_str());
        const char *step = "create";
        DWORD err = 0;
        {
          handle_t file {CreateFileW(temp_path.c_str(), GENERIC_WRITE, 0, &sa, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
          if (!file) {
            err = GetLastError();
          } else {
            std::size_t off = 0;
            while (off < content.size()) {
              const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(content.size() - off, 1u << 20));
              DWORD written = 0;
              if (!WriteFile(file.h, content.data() + off, chunk, &written, nullptr) || written == 0) {
                step = "write";
                err = GetLastError();
                if (err == 0) {
                  err = ERROR_WRITE_FAULT;
                }
                break;
              }
              off += written;
            }
          }
        }
        if (err == 0) {
          step = "rename";
          if (!MoveFileExW(temp_path.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            err = GetLastError();
          }
        }
        if (err != 0) {
          DeleteFileW(temp_path.c_str());
          return std::format("{}-{}", step, err);
        }
        return {};
      }

      /// 寫（或覆寫）一次執行的 summary.json；失敗只寫一行 warning，selftest 照常進行
      void save(const std::string &run_id, const nlohmann::json &j) {
        if (!platf::is_running_as_system()) {
          return;
        }
        const std::wstring id {run_id.begin(), run_id.end()};  // run id 只有 ASCII 數字、T、Z
        if (!valid_run_id(id)) {
          BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] summary-skip reason=run-id"sv;
          return;
        }
        const std::wstring config = platf::appdata().wstring();
        const std::wstring steamvr = config + L"\\steamvr";
        const std::wstring selftest = steamvr + L"\\selftest";
        const std::wstring run_dir = selftest + L"\\" + id;
        std::string why;
        const char *level = "config";
        if (why = check_dir(config, false); why.empty()) {
          level = "steamvr";
          if (why = ensure_dir(steamvr); why.empty()) {
            level = "selftest";
            if (why = ensure_dir(selftest); why.empty()) {
              level = "run";
              if (why = ensure_dir(run_dir); why.empty()) {
                level = "file";
                why = write_file(run_dir, dump_json(j));
              }
            }
          }
        }
        if (!why.empty()) {
          BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] summary-skip run="sv << run_id << " reason="sv << why << " level="sv << level;
          return;
        }
        BOOST_LOG(info) << "[VIPLE-VR-ADMIN] summary written run="sv << run_id << " state="sv << (j.contains("state") && j["state"].is_string() ? j["state"].get<std::string>() : "?"s);
      }

      bool is_int(const nlohmann::json &j, const char *key) {
        return j.contains(key) && j[key].is_number_integer();
      }

      /**
       * @brief 最近一份 summary.json（依 run id 的字典序＝時間序）。內容來自 protected 目錄，仍當資料驗證型別。
       * @param why 找不到或讀不了時的原因（"none"＝從來沒有存過）。
       */
      std::optional<nlohmann::json> load_latest(std::string &why) {
        const std::wstring config = platf::appdata().wstring();
        const std::wstring steamvr = config + L"\\steamvr";
        const std::wstring selftest = steamvr + L"\\selftest";
        if (GetFileAttributesW(selftest.c_str()) == INVALID_FILE_ATTRIBUTES) {
          why = "none";
          return std::nullopt;
        }
        for (const auto &[path, owned] : {std::pair {config, false}, std::pair {steamvr, true}, std::pair {selftest, true}}) {
          if (why = check_dir(path, owned); !why.empty()) {
            return std::nullopt;
          }
        }

        std::wstring best;
        WIN32_FIND_DATAW fd {};
        HANDLE find = FindFirstFileExW((selftest + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
        if (find != INVALID_HANDLE_VALUE) {
          do {
            const std::wstring_view name {fd.cFileName};
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && valid_run_id(name) && name > best) {
              best = std::wstring {name};
            }
          } while (FindNextFileW(find, &fd));
          FindClose(find);
        }
        if (best.empty()) {
          why = "none";
          return std::nullopt;
        }
        const std::wstring run_dir = selftest + L"\\" + best;
        if (why = check_dir(run_dir, true); !why.empty()) {
          return std::nullopt;
        }

        handle_t file {CreateFileW((run_dir + L"\\summary.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!file) {
          why = std::format("open-{}", GetLastError());
          return std::nullopt;
        }
        FILE_ATTRIBUTE_TAG_INFO tag {};
        LARGE_INTEGER size {};
        if (!GetFileInformationByHandleEx(file.h, FileAttributeTagInfo, &tag, sizeof(tag)) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !GetFileSizeEx(file.h, &size) || size.QuadPart <= 0 || static_cast<std::uint64_t>(size.QuadPart) > k_max_bytes) {
          why = "file";
          return std::nullopt;
        }
        std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t off = 0;
        while (off < text.size()) {
          DWORD n = 0;
          if (!ReadFile(file.h, text.data() + off, static_cast<DWORD>(std::min<std::size_t>(text.size() - off, 1u << 20)), &n, nullptr) || n == 0) {
            why = "read";
            return std::nullopt;
          }
          off += n;
        }
        auto j = nlohmann::json::parse(text, nullptr, false);
        const std::string expect_id {best.begin(), best.end()};
        if (j.is_discarded() || !j.is_object() || !is_int(j, "v") || j["v"].get<int>() != 1 || !j.contains("run") || !j["run"].is_string() ||
            j["run"].get<std::string>() != expect_id || !j.contains("state") || !j["state"].is_string()) {
          why = "format";
          return std::nullopt;
        }
        const std::string state = j["state"].get<std::string>();
        if (state != "running" && (state != "done" || !is_int(j, "rc") || !is_int(j, "pass") || !is_int(j, "fail"))) {
          why = "format";
          return std::nullopt;
        }
        return j;
      }
    }  // namespace summary_store

    /**
     * @brief overlapped 寫入一則訊息，最多等 timeout_ms；逾時或 stop 就 CancelIoEx（永遠不 FlushFileBuffers，sec-M8）。
     */
    bool write_message(HANDLE pipe, const std::string &msg, DWORD timeout_ms, HANDLE stop_evt) {
      if (msg.size() > k_max_msg * 16) {
        return false;
      }
      ovl_t w;
      DWORD n = 0;
      if (!WriteFile(pipe, msg.data(), static_cast<DWORD>(msg.size()), nullptr, &w.o)) {
        if (GetLastError() != ERROR_IO_PENDING) {
          return false;
        }
        HANDLE hs[2] = {w.evt.h, stop_evt};
        const DWORD count = stop_evt ? 2 : 1;
        const DWORD r = WaitForMultipleObjects(count, hs, FALSE, timeout_ms);
        if (r != WAIT_OBJECT_0) {
          CancelIoEx(pipe, &w.o);
          GetOverlappedResult(pipe, &w.o, &n, TRUE);
          return false;
        }
      }
      if (!GetOverlappedResult(pipe, &w.o, &n, FALSE)) {
        return false;
      }
      return n == msg.size();
    }

    enum class read_e {
      message,
      too_large,
      disconnected,
      timeout,
      stopped,
    };

    /**
     * @brief 一個 pending 的 overlapped 讀取（server 在 attached 模式下一直保留一個，才偵測得到 CLI 斷線）。
     */
    class pending_read_t {
    public:
      explicit pending_read_t(HANDLE pipe):
          pipe_ {pipe} {
        buf_.resize(k_max_msg);
      }

      pending_read_t(const pending_read_t &) = delete;
      pending_read_t &operator=(const pending_read_t &) = delete;

      ~pending_read_t() {
        cancel();
      }

      /// 發出讀取；立即完成或出錯時 event 也會被 set，交給 complete() 判斷
      void start() {
        ov_.reset();
        pending_ = true;
        if (!ReadFile(pipe_, buf_.data(), static_cast<DWORD>(buf_.size()), nullptr, &ov_.o)) {
          const DWORD err = GetLastError();
          if (err != ERROR_IO_PENDING && err != ERROR_MORE_DATA) {
            // 同步失敗（多半是 ERROR_BROKEN_PIPE）：event 不會被 set，自己 set 讓等待端醒來
            immediate_error_ = err;
            SetEvent(ov_.evt.h);
          }
        }
      }

      HANDLE event() const {
        return ov_.evt.h;
      }

      bool pending() const {
        return pending_;
      }

      /// event 被 set 之後呼叫
      read_e complete(std::string &out) {
        pending_ = false;
        if (immediate_error_) {
          immediate_error_ = 0;
          return read_e::disconnected;
        }
        DWORD n = 0;
        if (GetOverlappedResult(pipe_, &ov_.o, &n, FALSE)) {
          out.assign(buf_.data(), n);
          return read_e::message;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_MORE_DATA) {
          // 超過上限：把這則訊息剩下的部分讀掉丟棄（同步等，因為資料已經在 pipe 裡）
          drain_rest();
          return read_e::too_large;
        }
        return read_e::disconnected;
      }

      void cancel() {
        if (pending_ && !immediate_error_) {
          CancelIoEx(pipe_, &ov_.o);
          DWORD n = 0;
          GetOverlappedResult(pipe_, &ov_.o, &n, TRUE);
        }
        pending_ = false;
        immediate_error_ = 0;
      }

    private:
      void drain_rest() {
        for (int i = 0; i < 64; ++i) {
          ovl_t o;
          DWORD n = 0;
          BOOL ok = ReadFile(pipe_, buf_.data(), static_cast<DWORD>(buf_.size()), nullptr, &o.o);
          DWORD err = ok ? 0 : GetLastError();
          if (!ok && err == ERROR_IO_PENDING) {
            if (WaitForSingleObject(o.evt.h, 1000) != WAIT_OBJECT_0) {
              CancelIoEx(pipe_, &o.o);
              GetOverlappedResult(pipe_, &o.o, &n, TRUE);
              return;
            }
            ok = GetOverlappedResult(pipe_, &o.o, &n, FALSE);
            err = ok ? 0 : GetLastError();
          }
          if (ok || err != ERROR_MORE_DATA) {
            return;
          }
        }
      }

      HANDLE pipe_;
      std::vector<char> buf_;
      ovl_t ov_;
      bool pending_ = false;
      DWORD immediate_error_ = 0;
    };

    /// 讀一則訊息（阻塞，最多 timeout_ms；stop_evt 可為 nullptr）
    read_e read_one(HANDLE pipe, std::string &out, DWORD timeout_ms, HANDLE stop_evt) {
      pending_read_t rd {pipe};
      rd.start();
      HANDLE hs[2] = {rd.event(), stop_evt};
      const DWORD count = stop_evt ? 2 : 1;
      const DWORD r = WaitForMultipleObjects(count, hs, FALSE, timeout_ms);
      if (r == WAIT_OBJECT_0) {
        return rd.complete(out);
      }
      rd.cancel();
      return r == WAIT_TIMEOUT ? read_e::timeout : read_e::stopped;
    }

    // ── 一次 selftest 執行 ──────────────────────────────────────────────

    struct run_t {
      std::string id;
      bool detached = false;
      std::atomic<bool> abort {false};
      std::atomic<bool> attached_client {false};  ///< 還有 attached 的 CLI 在收行
      handle_t wake {CreateEventW(nullptr, FALSE, FALSE, nullptr)};  ///< 有新行或結束（auto-reset）

      std::mutex m;
      bool done = false;
      std::deque<std::string> outbox;  ///< 還沒送給 attached CLI 的行
      std::deque<std::string> history;  ///< vr-status 用
      std::size_t history_dropped = 0;
      vr::selftest::result_t result;

      std::thread worker;
    };

    class server_t {
    public:
      server_t() {
        stop_evt_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        thread_ = std::thread([this]() {
          thread_main();
        });
      }

      server_t(const server_t &) = delete;
      server_t &operator=(const server_t &) = delete;

      ~server_t() {
        SetEvent(stop_evt_.h);
        if (thread_.joinable()) {
          thread_.join();
        }

        std::shared_ptr<run_t> run;
        {
          std::lock_guard lk {mtx_};
          run = current_;
        }
        if (run) {
          run->abort.store(true);
          const auto deadline = std::chrono::steady_clock::now() + k_shutdown_wait;
          bool done = false;
          while (std::chrono::steady_clock::now() < deadline) {
            {
              std::lock_guard lk {run->m};
              done = run->done;
            }
            if (done) {
              break;
            }
            std::this_thread::sleep_for(50ms);
          }
          if (run->worker.joinable()) {
            if (done) {
              run->worker.join();
            } else {
              // 清理超過上限：不讓 service 的停止卡住（行程結束時 job 內的 vr_probe 會一起結束）
              BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] selftest run="sv << run->id << " did not finish within "sv << k_shutdown_wait.count() << " s of shutdown; detaching"sv;
              run->worker.detach();
            }
          }
        }

        std::lock_guard lk {mtx_};
        pipe_.reset();
      }

      HANDLE duplicate_pipe() {
        std::lock_guard lk {mtx_};
        if (!pipe_) {
          return nullptr;
        }
        HANDLE dup = nullptr;
        // 最小權限：T0 只讀 SD 與名稱（READ_CONTROL；FILE_READ_ATTRIBUTES 保守一併給），不給讀寫資料
        if (!DuplicateHandle(GetCurrentProcess(), pipe_.h, GetCurrentProcess(), &dup, READ_CONTROL | FILE_READ_ATTRIBUTES, FALSE, 0)) {
          return nullptr;
        }
        return dup;
      }

    private:
      bool stopping() const {
        return WaitForSingleObject(stop_evt_.h, 0) == WAIT_OBJECT_0;
      }

      /// 等 duration 或 stop；stop 時回 true
      bool wait_stop(std::chrono::milliseconds duration) const {
        return WaitForSingleObject(stop_evt_.h, static_cast<DWORD>(duration.count())) == WAIT_OBJECT_0;
      }

      /// @return 建立成功；失敗時 retry 表示值得稍後重試
      bool create_pipe(bool &retry) {
        retry = true;
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(k_pipe_sddl, SDDL_REVISION_1, &sd, nullptr)) {
          BOOST_LOG(error) << "[VIPLE-VR-ADMIN] pipe-create-failed reason=sddl-"sv << GetLastError();
          retry = false;
          return false;
        }
        SECURITY_ATTRIBUTES sa {sizeof(sa), sd, FALSE};
        HANDLE h = CreateNamedPipeW(
          k_pipe_name,
          PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
          PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
          1,
          k_pipe_out_buffer,
          k_max_msg,
          0,
          &sa
        );
        const DWORD err = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
        LocalFree(sd);
        if (h == INVALID_HANDLE_VALUE) {
          if (err == ERROR_INVALID_OWNER) {
            // 非 SYSTEM（例如開發時以 console 模式執行）：設不了 O:SY，這是預期的 fail closed，不重試
            BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] pipe disabled reason=not-system (admin pipe needs the SYSTEM service)"sv;
            retry = false;
          } else if (err == ERROR_ACCESS_DENIED || err == ERROR_PIPE_BUSY) {
            BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] pipe-squatted err="sv << err << " (retry in "sv << k_retry_interval.count() << " s)"sv;
          } else {
            BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] pipe-create-failed reason=error-"sv << err << " (retry in "sv << k_retry_interval.count() << " s)"sv;
          }
          return false;
        }
        std::lock_guard lk {mtx_};
        pipe_.reset(h);
        return true;
      }

      void thread_main() {
        platf::set_thread_name("vr_admin");
        while (!stopping()) {
          if (!pipe_) {
            bool retry = true;
            if (!create_pipe(retry)) {
              if (!retry) {
                WaitForSingleObject(stop_evt_.h, INFINITE);
                return;
              }
              if (wait_stop(k_retry_interval)) {
                return;
              }
              continue;
            }
            BOOST_LOG(info) << "[VIPLE-VR-ADMIN] pipe ready"sv;
          }

          ovl_t c;
          bool connected = false;
          if (ConnectNamedPipe(pipe_.h, &c.o)) {
            connected = true;
          } else {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
              connected = true;
            } else if (err == ERROR_IO_PENDING) {
              HANDLE hs[2] = {c.evt.h, stop_evt_.h};
              const DWORD r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
              if (r != WAIT_OBJECT_0) {
                CancelIoEx(pipe_.h, &c.o);
                DWORD n = 0;
                GetOverlappedResult(pipe_.h, &c.o, &n, TRUE);
                return;
              }
              DWORD n = 0;
              connected = GetOverlappedResult(pipe_.h, &c.o, &n, FALSE) != FALSE;
            } else if (err != ERROR_NO_DATA) {
              BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] connect-failed err="sv << err;
              if (wait_stop(1s)) {
                return;
              }
            }
          }

          if (connected) {
            // JSON 的型別錯誤等例外不能讓 pipe 執行緒（進而整個 server）結束
            try {
              handle_client();
            } catch (const std::exception &e) {
              BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] request failed: "sv << e.what();
            }
          }
          // instance 不關，只斷線後重新 Connect（不留搶名窗口）
          DisconnectNamedPipe(pipe_.h);
          reap_finished_run();
        }
      }

      void reap_finished_run() {
        std::lock_guard lk {mtx_};
        if (!current_) {
          return;
        }
        bool done;
        {
          std::lock_guard rlk {current_->m};
          done = current_->done;
        }
        if (done) {
          if (current_->worker.joinable()) {
            current_->worker.join();
          }
          last_ = std::move(current_);
          current_.reset();
        }
      }

      bool send(const nlohmann::json &j) {
        return write_message(pipe_.h, dump_json(j), k_write_timeout_ms, stop_evt_.h);
      }

      void send_result(int rc, std::string_view error, const nlohmann::json &extra = nlohmann::json::object()) {
        nlohmann::json j = extra;
        j["t"] = "result";
        j["rc"] = rc;
        if (!error.empty()) {
          j["error"] = std::string {error};
        }
        send(j);
      }

      /// 送完最後一則之後，等 CLI 讀完並關掉它那端（DisconnectNamedPipe 會丟掉還沒讀的資料）
      void wait_client_close(DWORD timeout_ms) {
        std::string ignored;
        for (int i = 0; i < 4; ++i) {
          const auto r = read_one(pipe_.h, ignored, timeout_ms, stop_evt_.h);
          if (r != read_e::message && r != read_e::too_large) {
            return;
          }
        }
      }

      /// 深度防禦：連線 token 必須是 BUILTIN\Administrators 成員而且是提升的（DACL＋ML HI 已在 kernel 擋掉非提升者）
      bool client_is_elevated_admin(DWORD &why) {
        HANDLE token = nullptr;
        {
          platf::impersonation_guard guard {platf::impersonation_guard::named_pipe_client, pipe_.h};
          if (!guard.valid()) {
            why = guard.error();
            return false;
          }
          if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
            why = GetLastError();
            token = nullptr;
          }
        }  // guard 在這裡 RevertToSelf
        if (!token) {
          return false;
        }
        handle_t tok {token};

        BYTE admins[SECURITY_MAX_SID_SIZE];
        DWORD sid_size = sizeof(admins);
        BOOL member = FALSE;
        if (!CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, admins, &sid_size) ||
            !CheckTokenMembership(tok.h, admins, &member)) {
          why = GetLastError();
          return false;
        }
        TOKEN_ELEVATION elevation {};
        DWORD len = 0;
        if (!GetTokenInformation(tok.h, TokenElevation, &elevation, sizeof(elevation), &len)) {
          why = GetLastError();
          return false;
        }
        why = 0;
        return member && elevation.TokenIsElevated;
      }

      void handle_client() {
        ULONG pid = 0;
        ULONG session = 0;
        GetNamedPipeClientProcessId(pipe_.h, &pid);
        GetNamedPipeClientSessionId(pipe_.h, &session);

        std::string text;
        const auto rr = read_one(pipe_.h, text, k_first_msg_timeout_ms, stop_evt_.h);
        if (rr != read_e::message) {
          if (rr == read_e::too_large) {
            BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] client dropped reason=too-large pid="sv << pid;
          } else if (rr != read_e::stopped) {
            BOOST_LOG(info) << "[VIPLE-VR-ADMIN] client dropped reason="sv << (rr == read_e::timeout ? "timeout"sv : "disconnected"sv) << " pid="sv << pid;
          }
          return;
        }

        DWORD why = 0;
        if (!client_is_elevated_admin(why)) {
          BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] rejected reason=not-elevated-admin pid="sv << pid << " session="sv << session << " err="sv << why;
          send_result(vr::selftest::rc_access_denied, "access denied (elevated administrator required)"sv);
          wait_client_close(2000);
          return;
        }

        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("cmd") || !j["cmd"].is_string() || j.value("v", 0) != 1) {
          BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] bad request pid="sv << pid;
          send_result(vr::selftest::rc_bad_request, "bad request (expected {\"v\":1,\"cmd\":...})"sv);
          wait_client_close(2000);
          return;
        }
        const std::string cmd = j["cmd"].get<std::string>();
        nlohmann::json args = nlohmann::json::object();
        if (auto it = j.find("args"); it != j.end()) {
          if (!it->is_object()) {
            send_result(vr::selftest::rc_bad_request, "args must be an object"sv);
            wait_client_close(2000);
            return;
          }
          args = *it;
        }
        BOOST_LOG(info) << "[VIPLE-VR-ADMIN] request cmd="sv << cmd << " pid="sv << pid << " session="sv << session;

        if (cmd == "vr-selftest") {
          cmd_selftest(args);
        } else if (cmd == "vr-status") {
          cmd_status();
        } else if (cmd == "abort") {
          cmd_abort();
        } else if (cmd == "steamvr-driver") {
          cmd_steamvr_driver(args);
        } else {
          send_result(vr::selftest::rc_bad_request, "unknown cmd"sv);
          wait_client_close(2000);
        }
      }

      void cmd_selftest(const nlohmann::json &args) {
        std::string err;
        auto req = vr::selftest::parse_request(dump_json(args), err);
        if (!req) {
          BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] selftest rejected reason=bad-args ("sv << err << ')';
          send_result(vr::selftest::rc_bad_request, err);
          wait_client_close(2000);
          return;
        }

        reap_finished_run();
        std::shared_ptr<run_t> run;
        {
          std::lock_guard lk {mtx_};
          if (current_) {
            send_result(vr::selftest::rc_busy, "another selftest is running (use --vr-status / --vr-abort)"sv);
            wait_client_close(2000);
            return;
          }
          run = std::make_shared<run_t>();
          run->id = utc_run_id();
          run->detached = req->detach;
          run->attached_client.store(!req->detach);
          current_ = run;
        }

        // 開跑前先落地 state=running：server 在執行中結束（部署、自我更新、當掉）時，之後的 --vr-status 能回報 interrupted
        persist_start(*run);
        run->worker = std::thread([run, request = *req]() {
          platf::set_thread_name("vr_selftest");
          auto sink = [run](const std::string &line) {
            std::lock_guard lk {run->m};
            if (run->attached_client.load()) {
              run->outbox.push_back(line);
            }
            run->history.push_back(line);
            if (run->history.size() > k_history_max) {
              run->history.pop_front();
              ++run->history_dropped;
            }
            SetEvent(run->wake.h);
          };
          auto result = vr::selftest::run(request, run->id, sink, run->abort);
          {
            std::lock_guard lk {run->m};
            run->result = std::move(result);
          }
          // 在 done 之前覆寫成 state=done：~server_t 只等 done（最多 15 s），abort 的結果也要落地
          persist_done(*run);
          {
            std::lock_guard lk {run->m};
            run->done = true;
          }
          SetEvent(run->wake.h);
        });

        if (run->detached) {
          BOOST_LOG(info) << "[VIPLE-VR-ADMIN] selftest accepted run="sv << run->id << " mode=detached"sv;
          send({{"t", "accepted"}, {"run", run->id}});
          wait_client_close(2000);
          return;
        }
        BOOST_LOG(info) << "[VIPLE-VR-ADMIN] selftest started run="sv << run->id << " mode=attached"sv;
        attached_loop(run);
      }

      /// attached：把行轉送給 CLI；CLI 斷線＝abort（sec-M13）
      void attached_loop(const std::shared_ptr<run_t> &run) {
        pending_read_t rd {pipe_.h};
        rd.start();
        bool client_gone = !send({{"t", "started"}, {"run", run->id}});
        bool result_sent = false;

        while (!client_gone) {
          HANDLE hs[3] = {rd.event(), run->wake.h, stop_evt_.h};
          const DWORD w = WaitForMultipleObjects(3, hs, FALSE, 500);
          if (w == WAIT_OBJECT_0 + 2) {
            run->abort.store(true);
            break;
          }
          if (w == WAIT_OBJECT_0) {
            std::string msg;
            const auto r = rd.complete(msg);
            if (r == read_e::disconnected) {
              client_gone = true;
              break;
            }
            if (r == read_e::message) {
              // attached 期間 CLI 只可能送 abort
              const auto j = nlohmann::json::parse(msg, nullptr, false);
              if (!j.is_discarded() && j.is_object() && j.contains("cmd") && j["cmd"].is_string() && j["cmd"].get<std::string>() == "abort") {
                BOOST_LOG(info) << "[VIPLE-VR-ADMIN] abort requested run="sv << run->id;
                run->abort.store(true);
              }
            }
            rd.start();
          }

          std::deque<std::string> lines;
          bool done;
          {
            std::lock_guard lk {run->m};
            lines.swap(run->outbox);
            done = run->done;
          }
          for (const auto &l : lines) {
            if (!send({{"t", "line"}, {"text", l}})) {
              client_gone = true;
              break;
            }
          }
          if (client_gone) {
            break;
          }
          if (done) {
            nlohmann::json extra {{"run", run->id}, {"pass", run->result.pass}, {"fail", run->result.fail}, {"notRun", run->result.not_run}};
            send_result(run->result.rc, run->result.error, extra);
            result_sent = true;
            break;
          }
        }

        run->attached_client.store(false);
        {
          std::lock_guard lk {run->m};
          run->outbox.clear();
        }

        if (client_gone && !result_sent) {
          BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] client disconnected run="sv << run->id << " -> abort"sv;
          run->abort.store(true);
          rd.cancel();
          return;
        }
        if (result_sent && rd.pending()) {
          // 等 CLI 讀完最後一則並關閉（pending read 以 ERROR_BROKEN_PIPE 完成）
          HANDLE hs[2] = {rd.event(), stop_evt_.h};
          if (WaitForMultipleObjects(2, hs, FALSE, 3000) == WAIT_OBJECT_0) {
            std::string ignored;
            rd.complete(ignored);
          }
        }
        rd.cancel();
      }

      void cmd_status() {
        std::shared_ptr<run_t> run;
        {
          std::lock_guard lk {mtx_};
          run = current_ ? current_ : last_;
        }
        if (!run) {
          // 記憶體裡沒有（例：service 在 --detach 執行後重啟）：讀最近一份 summary.json（ops-m17）
          std::string why;
          if (const auto saved = summary_store::load_latest(why)) {
            send_saved_status(*saved);
          } else if (why == "none") {
            send_result(vr::selftest::rc_status_none, "no selftest has run since the server started, and no saved summary exists"sv, {{"state", "none"}});
          } else {
            BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] saved summary unreadable reason="sv << why;
            send_result(vr::selftest::rc_status_none, "no selftest has run since the server started; the latest saved summary is unreadable (" + why + ")", {{"state", "none"}});
          }
          wait_client_close(2000);
          return;
        }
        std::deque<std::string> history;
        std::size_t dropped;
        bool done;
        vr::selftest::result_t result;
        {
          std::lock_guard lk {run->m};
          history = run->history;
          dropped = run->history_dropped;
          done = run->done;
          result = run->result;
        }
        bool ok = true;
        if (dropped) {
          ok = send({{"t", "line"}, {"text", std::format("[VIPLE-VR-ADMIN] ({} earlier lines dropped)", dropped)}});
        }
        for (const auto &l : history) {
          if (!ok) {
            break;
          }
          ok = send({{"t", "line"}, {"text", l}});
        }
        if (!ok) {
          return;
        }
        nlohmann::json extra {{"run", run->id}, {"state", done ? "done" : "running"}};
        if (done) {
          extra["pass"] = result.pass;
          extra["fail"] = result.fail;
          extra["notRun"] = result.not_run;
          send_result(result.rc, result.error, extra);
        } else {
          send_result(vr::selftest::rc_status_running, ""sv, extra);
        }
        wait_client_close(2000);
      }

      /// 回放 summary.json（load_latest 已驗過基本型別）：輸出行，然後 result；state=running 代表前一個 server 在執行中結束
      void send_saved_status(const nlohmann::json &j) {
        const std::string id = j["run"].get<std::string>();
        const std::string state = j["state"].get<std::string>();
        bool ok = true;
        if (summary_store::is_int(j, "history_dropped") && j["history_dropped"].get<int64_t>() > 0) {
          ok = send({{"t", "line"}, {"text", std::format("[VIPLE-VR-ADMIN] ({} earlier lines dropped)", j["history_dropped"].get<int64_t>())}});
        }
        if (j.contains("history") && j["history"].is_array()) {
          std::size_t n = 0;
          for (const auto &l : j["history"]) {
            if (!ok || ++n > k_history_max) {
              break;
            }
            if (l.is_string()) {
              std::string text = l.get<std::string>();
              if (text.size() > summary_store::k_max_line) {
                text.resize(summary_store::k_max_line);
              }
              ok = send({{"t", "line"}, {"text", text}});
            }
          }
        }
        if (!ok) {
          return;
        }
        BOOST_LOG(info) << "[VIPLE-VR-ADMIN] status from saved summary run="sv << id << " state="sv << state;
        if (state == "done") {
          nlohmann::json extra {{"run", id}, {"state", "done"}, {"source", "file"}, {"pass", j["pass"].get<int64_t>()}, {"fail", j["fail"].get<int64_t>()}};
          if (summary_store::is_int(j, "not_run")) {
            extra["notRun"] = j["not_run"].get<int64_t>();
          }
          const std::string error = j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : std::string {};
          send_result(static_cast<int>(j["rc"].get<int64_t>()), error, extra);
        } else {
          send_result(vr::selftest::rc_aborted, "interrupted: the server stopped while this selftest was running (no result was recorded)"sv, {{"run", id}, {"state", "interrupted"}, {"source", "file"}});
        }
      }

      static void persist_start(const run_t &run) {
        summary_store::save(run.id, {{"v", 1}, {"run", run.id}, {"state", "running"}, {"detached", run.detached}});
      }

      static void persist_done(run_t &run) {
        nlohmann::json j {{"v", 1}, {"run", run.id}, {"state", "done"}, {"detached", run.detached}, {"end_utc", utc_run_id()}};
        {
          std::lock_guard lk {run.m};
          j["rc"] = run.result.rc;
          j["pass"] = run.result.pass;
          j["fail"] = run.result.fail;
          j["not_run"] = run.result.not_run;
          j["error"] = run.result.error;
          j["history_dropped"] = run.history_dropped;
          auto history = nlohmann::json::array();
          for (const auto &l : run.history) {
            history.push_back(l);
          }
          j["history"] = std::move(history);
        }
        summary_store::save(run.id, j);
      }

      void cmd_abort() {
        std::shared_ptr<run_t> run;
        {
          std::lock_guard lk {mtx_};
          run = current_;
        }
        if (!run) {
          send_result(vr::selftest::rc_status_none, "no selftest is running"sv);
        } else {
          BOOST_LOG(info) << "[VIPLE-VR-ADMIN] abort requested run="sv << run->id;
          run->abort.store(true);
          send_result(vr::selftest::rc_ok, ""sv, {{"run", run->id}});
        }
        wait_client_close(2000);
      }

      void cmd_steamvr_driver(const nlohmann::json &args) {
        const std::string op = args.contains("op") && args["op"].is_string() ? args["op"].get<std::string>() : std::string {};
        if (op == "uninstall") {
          send_result(vr::selftest::rc_unsupported, "steamvr-driver uninstall is not implemented until V5"sv);
          wait_client_close(2000);
          return;
        }
        if (op != "status") {
          send_result(vr::selftest::rc_bad_request, "op must be status|uninstall"sv);
          wait_client_close(2000);
          return;
        }

        // ── 與 vr_bridge（S1-07）的接縫 ──
        const auto s = vr::bridge::status();
        const uint32_t v = s.driver_version_packed;
        const std::string driver = v ? std::format("{}.{}.{}", v >> 24, (v >> 16) & 0xFF, v & 0xFFFF) : "none"s;
        const std::string line = std::format(
          "[VIPLE-VR-ADMIN] steamvr-driver status bridge={} phase={} pipe={} gen={} ready={} peer={} driver={} driver_state={} handshakes={} rejects={}",
          s.connected ? "connected"sv : "idle"sv,
          vr::bridge::phase_name(s.phase),
          s.pipe_reason,
          s.generation,
          s.ready ? 1 : 0,
          s.connected ? (s.peer_is_selftest ? "selftest-peer"sv : "vrserver"sv) : "none"sv,
          driver,
          s.driver_state,
          s.handshakes,
          s.rejects
        );
        BOOST_LOG(info) << line;
        if (send({{"t", "line"}, {"text", line}})) {
          send({{"t", "line"}, {"text", "[VIPLE-VR-ADMIN] steamvr-driver registration/guard status: not implemented until V5"s}});
          send_result(vr::selftest::rc_ok, ""sv);
        }
        wait_client_close(2000);
      }

      handle_t stop_evt_;
      std::thread thread_;
      std::mutex mtx_;  ///< 保護 pipe_（duplicate_pipe 會從別的執行緒讀）、current_、last_
      handle_t pipe_;
      std::shared_ptr<run_t> current_;
      std::shared_ptr<run_t> last_;
    };

    std::mutex g_server_mtx;
    server_t *g_server = nullptr;

    class services_t: public platf::deinit_t {
    public:
      services_t() {
        // ── 與 vr_bridge（S1-07）的接縫：VR pipe 在 server 一啟動就建立（K3，DACL 先只有 SY）──
        // start() 回 false（非 SYSTEM、session 不符、被搶名…）時 bridge 自己記原因；被搶名時它可能留著
        // 30 s 重試的執行緒，所以解構時不論 start() 的結果都呼叫 stop()（stop() 必須可以安全地重複／在失敗後呼叫）。
        if (!vr::bridge::start()) {
          BOOST_LOG(warning) << "[VIPLE-VR-ADMIN] vr bridge did not start (see [VIPLE-VR-IPC] lines); the admin pipe still starts for diagnostics"sv;
        }
        auto server = std::make_unique<server_t>();
        std::lock_guard lk {g_server_mtx};
        g_server = server.release();
      }

      ~services_t() override {
        server_t *server;
        {
          std::lock_guard lk {g_server_mtx};
          server = g_server;
          g_server = nullptr;
        }
        delete server;  // 先中止 selftest（它會用到 bridge），再停 admin pipe
        vr::bridge::stop();
      }
    };

    // ── CLI ─────────────────────────────────────────────────────────────

    /// 連 admin pipe：成功回 handle；失敗時印出原因、rc 填好
    HANDLE cli_open(int &rc) {
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      for (;;) {
        HANDLE h = CreateFileW(
          k_pipe_name,
          GENERIC_READ | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES,
          0,
          nullptr,
          OPEN_EXISTING,
          FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
          nullptr
        );
        if (h != INVALID_HANDLE_VALUE) {
          // 先驗 pipe 擁有者是 SYSTEM 再送任何東西（搶名者建不出 owner=SY 的 pipe）
          PSID owner = nullptr;
          PSECURITY_DESCRIPTOR sd = nullptr;
          const DWORD err = GetSecurityInfo(h, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr, nullptr, &sd);
          const bool owner_ok = err == ERROR_SUCCESS && owner && IsWellKnownSid(owner, WinLocalSystemSid);
          if (sd) {
            LocalFree(sd);
          }
          if (!owner_ok) {
            CloseHandle(h);
            BOOST_LOG(error) << "[VIPLE-VR-ADMIN] refusing to talk to the admin pipe: its owner is not SYSTEM (err="sv << err << "); another process may have squatted the name"sv;
            rc = vr::selftest::rc_transport;
            return nullptr;
          }
          DWORD mode = PIPE_READMODE_MESSAGE;
          if (!SetNamedPipeHandleState(h, &mode, nullptr, nullptr)) {
            BOOST_LOG(error) << "[VIPLE-VR-ADMIN] SetNamedPipeHandleState failed err="sv << GetLastError();
            CloseHandle(h);
            rc = vr::selftest::rc_transport;
            return nullptr;
          }
          return h;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_BUSY && std::chrono::steady_clock::now() < deadline) {
          WaitNamedPipeW(k_pipe_name, 2000);
          continue;
        }
        if (err == ERROR_ACCESS_DENIED) {
          BOOST_LOG(error) << "[VIPLE-VR-ADMIN] access denied（需要提升的管理員）: run this command from an elevated administrator shell"sv;
          rc = vr::selftest::rc_access_denied;
        } else if (err == ERROR_FILE_NOT_FOUND) {
          BOOST_LOG(error) << "[VIPLE-VR-ADMIN] admin pipe not found: the VipleStreamServer service is not running, vr_pcvr=disabled, or the server is not running as SYSTEM"sv;
          rc = vr::selftest::rc_transport;
        } else if (err == ERROR_PIPE_BUSY) {
          BOOST_LOG(error) << "[VIPLE-VR-ADMIN] admin pipe busy: another CLI is attached (an attached selftest can only be stopped from its own console, or wait for it)"sv;
          rc = vr::selftest::rc_busy;
        } else {
          BOOST_LOG(error) << "[VIPLE-VR-ADMIN] cannot open the admin pipe err="sv << err;
          rc = vr::selftest::rc_transport;
        }
        return nullptr;
      }
    }

    /// CLI 讀一則訊息（可跨多次 ERROR_MORE_DATA 組合）；timeout_ms=INFINITE 表示一直等
    read_e cli_read(HANDLE h, std::string &out, DWORD timeout_ms) {
      out.clear();
      std::vector<char> buf(65536);
      for (int part = 0; part < 64; ++part) {
        ovl_t o;
        DWORD n = 0;
        BOOL ok = ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), nullptr, &o.o);
        DWORD err = ok ? 0 : GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
          if (WaitForSingleObject(o.evt.h, timeout_ms) != WAIT_OBJECT_0) {
            CancelIoEx(h, &o.o);
            GetOverlappedResult(h, &o.o, &n, TRUE);
            return read_e::timeout;
          }
          ok = GetOverlappedResult(h, &o.o, &n, FALSE);
          err = ok ? 0 : GetLastError();
        } else if (ok || err == ERROR_MORE_DATA) {
          GetOverlappedResult(h, &o.o, &n, FALSE);
        }
        if (ok) {
          out.append(buf.data(), n);
          return read_e::message;
        }
        if (err == ERROR_MORE_DATA) {
          out.append(buf.data(), n);
          continue;
        }
        return read_e::disconnected;
      }
      return read_e::too_large;
    }

    /**
     * @brief 送出一個請求並把回應印出來。
     * @param first_timeout_ms 第一則回應的上限；stream_timeout_ms 之後每一則的上限（attached selftest 用 INFINITE）。
     */
    int cli_request_impl(const nlohmann::json &request, DWORD first_timeout_ms, DWORD stream_timeout_ms) {
      int rc = vr::selftest::rc_transport;
      handle_t h {cli_open(rc)};
      if (!h) {
        return rc;
      }
      if (!write_message(h.h, dump_json(request), 5000, nullptr)) {
        BOOST_LOG(error) << "[VIPLE-VR-ADMIN] failed to send the request err="sv << GetLastError();
        return vr::selftest::rc_transport;
      }

      DWORD timeout = first_timeout_ms;
      for (;;) {
        std::string text;
        const auto r = cli_read(h.h, text, timeout);
        if (r != read_e::message) {
          BOOST_LOG(error) << "[VIPLE-VR-ADMIN] connection to the server "sv << (r == read_e::timeout ? "timed out"sv : "was lost"sv) << " before a result arrived"sv;
          return vr::selftest::rc_transport;
        }
        timeout = stream_timeout_ms;
        const auto j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded() || !j.is_object() || (j.contains("t") && !j["t"].is_string()) || (j.contains("text") && !j["text"].is_string()) ||
            (j.contains("rc") && !j["rc"].is_number_integer())) {
          BOOST_LOG(error) << "[VIPLE-VR-ADMIN] malformed response from the server"sv;
          return vr::selftest::rc_transport;
        }
        const std::string t = j.value("t", std::string {});
        if (t == "line") {
          BOOST_LOG(info) << j.value("text", std::string {});
        } else if (t == "started") {
          BOOST_LOG(info) << "[VIPLE-VR-ADMIN] selftest started run="sv << j.value("run", std::string {}) << " (Ctrl+C aborts it)"sv;
        } else if (t == "accepted") {
          BOOST_LOG(info) << "[VIPLE-VR-ADMIN] selftest accepted run="sv << j.value("run", std::string {}) << " (detached; use --vr-status for progress and results, --vr-abort to stop)"sv;
          return vr::selftest::rc_ok;
        } else if (t == "result") {
          const int result_rc = j.value("rc", vr::selftest::rc_transport);
          std::string summary = std::format("[VIPLE-VR-ADMIN] result rc={}", result_rc);
          if (j.contains("state")) {
            summary += " state=" + j.value("state", std::string {});
          }
          if (j.contains("run")) {
            summary += " run=" + j.value("run", std::string {});
          }
          if (j.contains("pass")) {
            summary += std::format(" pass={} fail={}", j.value("pass", 0), j.value("fail", 0));
          }
          if (j.contains("notRun")) {
            summary += std::format(" notRun={}", j.value("notRun", 0));
          }
          if (j.contains("source")) {
            summary += " source=" + j.value("source", std::string {});
          }
          if (j.contains("error")) {
            summary += " error=" + j.value("error", std::string {});
          }
          if (result_rc == vr::selftest::rc_ok || result_rc == vr::selftest::rc_status_running) {
            BOOST_LOG(info) << summary;
          } else {
            BOOST_LOG(error) << summary;
          }
          return result_rc;
        }
      }
    }

    int cli_request(const nlohmann::json &request, DWORD first_timeout_ms, DWORD stream_timeout_ms) {
      try {
        return cli_request_impl(request, first_timeout_ms, stream_timeout_ms);
      } catch (const std::exception &e) {
        BOOST_LOG(error) << "[VIPLE-VR-ADMIN] malformed response from the server: "sv << e.what();
        return vr::selftest::rc_transport;
      }
    }

    bool no_extra_args(const char *what, int argc, char **argv) {
      if (argc == 0) {
        return true;
      }
      BOOST_LOG(error) << "[VIPLE-VR-ADMIN] "sv << what << " takes no arguments (got '"sv << (argv[0] ? argv[0] : "") << "')"sv;
      return false;
    }
  }  // namespace

  std::unique_ptr<platf::deinit_t> start_services() {
    return std::make_unique<services_t>();
  }

  HANDLE duplicate_admin_pipe_for_selftest() {
    std::lock_guard lk {g_server_mtx};
    return g_server ? g_server->duplicate_pipe() : nullptr;
  }

  int cli_vr_selftest(int argc, char **argv) {
    std::string err;
    const auto args = vr::selftest::cli_args_to_json(argc, argv, err);
    if (!args) {
      BOOST_LOG(error) << "[VIPLE-VR-ADMIN] --vr-selftest: "sv << err;
      return vr::selftest::rc_bad_request;
    }
    auto parsed = nlohmann::json::parse(*args, nullptr, false);
    // CLI 先做一次同樣的語意驗證，錯字不必連到 server 才知道
    if (!vr::selftest::parse_request(*args, err)) {
      BOOST_LOG(error) << "[VIPLE-VR-ADMIN] --vr-selftest: "sv << err;
      return vr::selftest::rc_bad_request;
    }
    const nlohmann::json request {{"v", 1}, {"cmd", "vr-selftest"}, {"args", parsed}};
    return cli_request(request, 15000, INFINITE);
  }

  int cli_vr_status(int argc, char **argv) {
    if (!no_extra_args("--vr-status", argc, argv)) {
      return vr::selftest::rc_bad_request;
    }
    return cli_request({{"v", 1}, {"cmd", "vr-status"}}, 15000, 15000);
  }

  int cli_vr_abort(int argc, char **argv) {
    if (!no_extra_args("--vr-abort", argc, argv)) {
      return vr::selftest::rc_bad_request;
    }
    return cli_request({{"v", 1}, {"cmd", "abort"}}, 15000, 15000);
  }

  int cli_steamvr_driver(int argc, char **argv) {
    std::string op;
    for (int i = 0; i < argc; ++i) {
      const std::string_view a = argv[i] ? std::string_view {argv[i]} : std::string_view {};
      if (a == "--wait-steamvr"sv) {
        // V5 才用得到（uninstall 等 SteamVR 結束的上限）；V2 只接受並忽略
        if (i + 1 >= argc) {
          BOOST_LOG(error) << "[VIPLE-VR-ADMIN] --wait-steamvr needs a value"sv;
          return vr::selftest::rc_bad_request;
        }
        ++i;
      } else if (a.starts_with("--wait-steamvr="sv)) {
        continue;
      } else if (op.empty() && (a == "status"sv || a == "uninstall"sv)) {
        op = std::string {a};
      } else {
        BOOST_LOG(error) << "[VIPLE-VR-ADMIN] --steamvr-driver: unexpected argument '"sv << a << "' (usage: --steamvr-driver status|uninstall [--wait-steamvr S])"sv;
        return vr::selftest::rc_bad_request;
      }
    }
    if (op.empty()) {
      BOOST_LOG(error) << "[VIPLE-VR-ADMIN] usage: --steamvr-driver status|uninstall [--wait-steamvr S]"sv;
      return vr::selftest::rc_bad_request;
    }
    if (op == "uninstall") {
      // K21：CLI 不自己碰 Steam 樹；註銷要由執行中的 server 以使用者身分做（V5 的編排器）
      BOOST_LOG(error) << "[VIPLE-VR-ADMIN] --steamvr-driver uninstall: not implemented until V5"sv;
      return vr::selftest::rc_unsupported;
    }
    return cli_request({{"v", 1}, {"cmd", "steamvr-driver"}, {"args", {{"op", op}}}}, 15000, 15000);
  }
}  // namespace platf::vr_admin
