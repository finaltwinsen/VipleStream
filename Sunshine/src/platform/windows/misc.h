/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <system_error>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);
  HDESK syncThreadDesktop();

  // ── 使用者身分工具（M1b S1-13 的 V2 部分：只補宣告，實作一直在 misc.cpp）──────────

  /**
   * @brief 取得主控台 session 使用者的 primary token（`WTSGetActiveConsoleSessionId` → `WTSQueryUserToken`）。
   * @details 需要 SYSTEM（SeTcbPrivilege）。`elevated=false` 回傳原樣的 token：UAC 管理員是 limited token
   *          （medium IL，`TokenElevationTypeLimited`）；`elevated=true` 且使用者是 UAC 管理員時改回 linked token。
   *          沒有人登入、或 API 失敗時回 nullptr。呼叫端負責 `CloseHandle`。
   * @param elevated 是否要提升的 token。
   * @return token；失敗時 nullptr。
   */
  HANDLE retrieve_users_token(bool elevated);

  /**
   * @brief 目前行程的 token 是否包含 LocalSystem（S-1-5-18）。
   */
  bool is_running_as_system();

  /**
   * @brief 以 `user_token` 模擬後執行 callback，結束時一定還原（內部用 impersonation_guard）。
   * @details M1b sec-M11：callback 丟出的例外在模擬範圍內被攔下並記錄（不會帶著使用者身分離開這個函式），
   *          此時回傳 `std::errc::interrupted`。模擬失敗回 `std::errc::permission_denied`，callback 不會被呼叫。
   *          模擬期間的限制見 docs（不得呼叫不帶 token 的 CreateProcessW、不得載入使用者可寫路徑的模組、不得用 COM）。
   * @param user_token primary 或 impersonation token（`TOKEN_QUERY | TOKEN_DUPLICATE`）。
   * @param callback 在模擬身分下執行的工作。
   * @return 錯誤碼；成功時為空。
   */
  std::error_code impersonate_current_user(HANDLE user_token, std::function<void()> callback);

  /**
   * @brief 模擬的 RAII 守衛（M1b sec-M11）：建構時開始模擬，解構時 `RevertToSelf`。
   * @details
   *   - `impersonation_guard(token)`：`ImpersonateLoggedOnUser(token)`。
   *   - `impersonation_guard(named_pipe_client, pipe)`：`ImpersonateNamedPipeClient(pipe)`（必須先從 pipe 讀過一則訊息）。
   *   - 模擬失敗時 `valid() == false`、`error()` 是 Win32 錯誤碼，解構時什麼都不做。
   *   - 解構時 `RevertToSelf` 失敗 → 記 fatal、flush log 後 `std::terminate()`：執行緒帶著未知身分繼續跑
   *     比結束行程更危險（取代舊版的 `DebugBreak()`）。
   *   - 只能在同一條執行緒上建構與解構（模擬是執行緒層級的狀態），所以不可複製、不可移動；請當區域變數用。
   */
  class impersonation_guard {
  public:
    struct named_pipe_client_t {};
    static constexpr named_pipe_client_t named_pipe_client {};

    explicit impersonation_guard(HANDLE user_token);
    impersonation_guard(named_pipe_client_t, HANDLE pipe);
    ~impersonation_guard();

    impersonation_guard(const impersonation_guard &) = delete;
    impersonation_guard &operator=(const impersonation_guard &) = delete;
    impersonation_guard(impersonation_guard &&) = delete;
    impersonation_guard &operator=(impersonation_guard &&) = delete;

    /// 模擬是否成功（成功時解構會 RevertToSelf）
    bool valid() const {
      return active_;
    }

    /// 模擬失敗時的 Win32 錯誤碼；成功時為 0
    DWORD error() const {
      return error_;
    }

  private:
    bool active_ = false;
    DWORD error_ = 0;
  };

  int64_t qpc_counter();

  /**
   * @brief QueryPerformanceFrequency 的值（每秒 QPC tick 數）。
   * @details M1b S1-01：開機後固定不變，第一次呼叫時取值並快取（函式區域 static，初始化執行緒安全）。
   *          兩台實測機都是 10 MHz，但程式一律不假設這個值。
   * @return 每秒 tick 數；取不到時回 0（Windows XP 起不會發生）。
   */
  int64_t qpc_frequency();

  /**
   * @brief 把 QPC tick 數（通常是兩個 QPC 值的差）換成 ns，全程整數運算。
   * @details (t / f) * 1e9 + (t % f) * 1e9 / f：不經 double。|ticks| 約 ≤ 9.2e9 × f（10 MHz 時約 292 年）
   *          時結果精確；超出就對稱飽和到 ±INT64_MAX，任何輸入都不會有有號溢位的 UB。餘數項要求
   *          f < 9.2e9（實際的 QPF 遠小於此）。
   *          C++ 整數除法向 0 截斷、飽和也對稱，所以 qpc_ticks_to_ns(-t) == -qpc_ticks_to_ns(t)。
   *          頻率取不到（0）時回 0。
   * @param ticks QPC tick 數，可為負。
   * @return 對應的 ns 數。
   */
  int64_t qpc_ticks_to_ns(int64_t ticks);

  /**
   * @brief 把 100 ns 單位的系統相對時間換成 QPC tick。
   * @details M1b S1-01（ops-m3）：WGC 的 Direct3D11CaptureFrame::SystemRelativeTime() 是 TimeSpan
   *          （100 ns 單位、QPC 時基），不是 raw QPC tick；QPF 恰好是 10 MHz 時兩者數值相同，
   *          這裡明確換算，不再依賴這個巧合。整數公式同 qpc_ticks_to_ns()，超出範圍同樣對稱飽和到
   *          ±INT64_MAX（f == 1e7 時，未飽和的範圍內結果恆等於輸入；只有 |t100ns| 距 INT64_MAX 不到約 1e7
   *          的極端值才會飽和）。
   * @param t100ns 100 ns 單位的時間。
   * @return QPC tick。
   */
  int64_t qpc_from_100ns(int64_t t100ns);

  /**
   * @brief 兩個 QPC 值的時間差（performance_counter1 − performance_counter2）。
   * @details M1b S1-01（K9）：舊版公式寫成 Δtick × 頻率 / 1e9，方向反了（10 MHz 時小 10⁴ 倍，
   *          所有由它換算的 frame_timestamp 實際上都 ≈ 換算當下）。現在 = qpc_ticks_to_ns(Δtick)；
   *          簽章不變。差值以 unsigned 相減後轉回 int64，再經 qpc_ticks_to_ns() 的飽和，整條路徑對任何輸入
   *          都沒有有號溢位的 UB；極端輸入會得到 ±INT64_MAX ns（差值超出 int64 時則是回繞後、無意義的值）。
   *          呼叫端做 `steady_now - age` 之前仍要自己檢查範圍（例如 §B.8 第 5 條把 present_qpc 限制在
   *          now ± 1 s）。
   */
  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Get file version information from a Windows executable or driver file.
   * @param file_path Path to the file to query.
   * @param version_str Output parameter for version string in format "major.minor.build.revision".
   * @return true if version info was successfully extracted, false otherwise.
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);
}  // namespace platf
