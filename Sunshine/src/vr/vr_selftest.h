/**
 * @file src/vr/vr_selftest.h
 * @brief VipleStream §VR（M1b S1-18）：`--vr-selftest` 的平台中立協調器。
 *
 * 權威設計：M1b 設計 §F.5（admin pipe 與 `--vr-selftest`）、§A.1 S1-18、K15、K16。
 *
 * 分工：
 *   - 這個模組：請求解析與驗證（CLI 參數 → JSON、JSON → request_t）、執行前提與 VR 預約（K15）、
 *     子測試排程、結果統計、`[VIPLE-VR-SELFTEST]` 行的格式；T6 的平台中立部分（vr_clock 估計器情境）。
 *   - 平台層（`namespace platform`）：Windows 在 `src/platform/windows/vr_selftest_win.cpp`
 *     （T0 安全描述元、T2 以 vr_probe ipcpeer 驗 IPC、T6 的 QPC／ABI 表／driver 端 unit）；
 *     其他平台在本模組的 .cpp 內以「不支援」實作。
 *   - 傳輸：Windows 的 admin pipe（`src/platform/windows/vr_admin_pipe.cpp`）在背景執行緒呼叫 run()，
 *     每一行經 sink 送回 CLI。
 *
 * V2 實作 T0、T2、T6，V3 加上 T1、T1b 與 T2.live，V4 加上 T4（--manual-steamvr）；其他子測試與選項回「not implemented until V<n>」（rc = rc_bad_request）。
 *
 * log 衛生（S1-02）：行內不含 handle 值、完整 GUID、SID（只印 RID）或任何機密。
 */
#pragma once

// standard includes
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vr::selftest {

  // ── 回傳碼（CLI 的 exit code 與 admin pipe result.rc 共用）──────────────────
  inline constexpr int rc_ok = 0;  ///< 選到的子測試全部通過
  inline constexpr int rc_failed = 1;  ///< 至少一項檢查失敗
  inline constexpr int rc_unsupported = 2;  ///< 平台不支援、vr_pcvr=disabled、或功能尚未實作（V5 之前的 uninstall）
  inline constexpr int rc_busy = 3;  ///< 已經有一個指令在執行
  inline constexpr int rc_session_active = 4;  ///< 有串流 session 或 VR 預約，selftest 不開始（或中途被搶先而中止）
  inline constexpr int rc_access_denied = 5;  ///< 不是提升的管理員（CLI 端 CreateFileW 被 kernel 擋，或 server 端深度防禦）
  inline constexpr int rc_aborted = 6;  ///< CLI 斷線、`--vr-abort` 或 server 關閉
  inline constexpr int rc_bad_request = 7;  ///< 參數錯誤或子測試尚未實作
  inline constexpr int rc_transport = 8;  ///< CLI 連不上 admin pipe、pipe 擁有者不是 SYSTEM、或連線中斷
  inline constexpr int rc_status_running = 9;  ///< `--vr-status`：還在執行
  inline constexpr int rc_status_none = 10;  ///< `--vr-status`／`--vr-abort`：沒有執行中或已完成的紀錄

  /**
   * @brief 已驗證的 selftest 請求（§F.5 的 `args`）。
   *
   * 目前接受 T0、T1、T1b、T2、T6；T3 以後的選項（cycles、probe、motion…）保留欄位，但非預設值會被拒絕，
   * 避免使用者以為它們生效了。
   */
  struct request_t {
    std::vector<std::string> only;  ///< 正規化後（大寫）的子測試，依 T0、T2、T6 的固定順序執行
    int cycles = 20;
    std::string probe = "none";
    std::string motion = "still";
    int hold_sec = 0;
    bool reset_seated = false;
    std::string preset = "none";
    bool dry_run = false;
    bool manual_steamvr = false;
    bool attach_session = false;
    bool detach = false;
    /// vr_probe 的完整路徑；空字串＝預設 `<install>\tools\vr_probe\vr_probe.exe`。只能由 CLI 旗標 `--probe-path` 指定（不看環境變數）。
    std::string probe_path;
  };

  /// 一行輸出（已含 `[VIPLE-VR-SELFTEST]` 前綴）；admin pipe 用它把行送回 CLI。
  using sink_t = std::function<void(const std::string &line)>;

  struct result_t {
    int rc = rc_ok;
    int pass = 0;
    int fail = 0;
    int not_run = 0;  ///< 明列延到後面切片才驗的情境（`result=NOT-RUN until=V<n>`）；不影響 rc，但一定出現在 (final) 行
    std::string error;  ///< 沒跑任何子測試時的原因（英文短句，無機密）
  };

  /**
   * @brief `--vr-selftest` 之後的 CLI 參數 → admin pipe 請求的 `args`（JSON 物件文字）。
   * @details 只做語法層級的轉換（旗標名稱、整數）；語意驗證在 server 端的 parse_request()，
   *          兩邊規則一致，CLI 先擋掉明顯的錯字。
   * @param argc `config::sunshine.cmd.argc`。
   * @param argv `config::sunshine.cmd.argv`。
   * @param err 失敗時的說明。
   * @return JSON 文字；失敗時 nullopt。
   */
  std::optional<std::string> cli_args_to_json(int argc, char **argv, std::string &err);

  /**
   * @brief admin pipe 收到的 `args`（JSON 物件文字）→ 驗證後的 request。
   * @param args_json `{"only":"T0,T2,T6", ...}`；空字串視為 `{}`（全部預設值）。
   * @param err 失敗時的說明（英文，會原樣回給 CLI）。
   */
  std::optional<request_t> parse_request(std::string_view args_json, std::string &err);

  /**
   * @brief 在呼叫端的執行緒上跑完一次 selftest（阻塞）。
   * @details
   *   - 前提（K15、§F.5）：平台支援、`vr_pcvr != disabled`、沒有串流 session、沒有其他 client 的 VR session 或預約；
   *     不符合就不開始（rc_unsupported／rc_session_active）。
   *   - 執行期間以 owner=`selftest` 持有 VR 預約並每秒續期，所有 `/launch`（含桌面）都會被 nvhttp 以 503 VR_BUSY 擋下；
   *     每秒重查 `rtsp_stream::session_count()`，出現串流 session 就中止並還原（rc_session_active）。
   *   - `abort` 變成 true 後在下一個檢查點停止（子測試內部以 50 ms 粒度輪詢），跑完清理再返回（rc_aborted）。
   *   - 每一行同時寫進 sunshine.log（大量明細例外，只送 sink）；最後一行是
   *     `[VIPLE-VR-SELFTEST] (final) pass=<n> fail=<n> notRun=<n> rc=<rc> ms=<n>`。
   * @param req 已驗證的請求。
   * @param run_id 這次執行的識別（UTC 時間字串），只用於 log。
   * @param sink 每一行輸出；可以是空的 std::function。
   * @param abort 外部中止旗標。
   */
  result_t run(const request_t &req, const std::string &run_id, const sink_t &sink, const std::atomic<bool> &abort);

  /// 目前是否有 selftest 在執行（nvhttp 可以據此把 503 訊息寫成 `VR_BUSY: selftest running`）
  bool running();

  /**
   * @brief 子測試的結果輸出器：統計 PASS／FAIL，並把每一行送到 sink 與 sunshine.log。
   *
   * 行格式：`[VIPLE-VR-SELFTEST] <id> result=PASS|FAIL|INFO|NOT-RUN <key=value ...>`；id 例如 `T0.pipe-sd`、`T2.handshake`。
   * NOT-RUN：設計列為該子測試的情境、但要到後面的切片才能驗（例：T2 的 gpu-hold 與 live 消費在 V3）。它不算 PASS
   * 也不算 FAIL，只計數並出現在 `T<n> end` 與 `(final)` 行的 `notRun=`，讓 rc=0 不會被誤讀成「全部情境都驗過」。
   * 只在執行 run() 的那條執行緒上使用（不做同步）。
   */
  class reporter_t {
  public:
    explicit reporter_t(sink_t sink);

    /// 一項檢查；失敗時 detail 要寫出「期望 vs 實際」
    void check(std::string_view id, bool pass, std::string_view detail);

    /// 不計分的資訊行（環境、實測數值、UNVERIFIED 項目的觀察結果）
    void info(std::string_view id, std::string_view detail);

    /// 設計要求、但這個切片還不能驗的情境：`<id> result=NOT-RUN until=<slice> <detail>`（計入 not_run，不影響 rc）
    void not_run(std::string_view id, std::string_view until, std::string_view detail);

    /// 大量明細（例如 ABI 表的 305 列）：只送 sink，不寫 sunshine.log
    void verbose(std::string_view id, std::string_view text);

    /// 任意一行（已經是完整內容，不含前綴）：同時寫 log 與 sink
    void line(std::string_view text);

    int passed() const {
      return pass_;
    }

    int failed() const {
      return fail_;
    }

    int not_run_count() const {
      return not_run_;
    }

  private:
    void emit(const std::string &line, bool to_log, bool is_failure);

    sink_t sink_;
    int pass_ = 0;
    int fail_ = 0;
    int not_run_ = 0;
  };

  /**
   * @brief 平台層的子測試（Windows 實作在 vr_selftest_win.cpp；其他平台在 vr_selftest.cpp 內回報不支援）。
   *
   * 每個函式都在 run() 的執行緒上被呼叫；`stop` 變成 true 時要盡快結束（結束前把自己啟動的行程與
   * bridge 的狀態還原）。
   */
  namespace platform {
    /// 這個平台能不能跑 selftest
    bool supported();

    /// T0：環境（session、主控台使用者、elevationType）、VR pipe 與 admin pipe 的安全描述元（含一次 SetSecurityInfo 來回）
    void run_t0(reporter_t &r, const request_t &req, const std::atomic<bool> &stop);

    /// T1（M1b V3）：display_vr_t 探測形態（is_hdr、get_hdr_metadata、alloc_img、dummy_img、complete_img）＋
    ///     VR 形狀的 encoder 探測（intra refresh 真的開起來）；結束時還原桌面探測狀態
    void run_t1(reporter_t &r, const request_t &req, const std::atomic<bool> &stop);

    /// T1b（M1b V3）：VR 探測前後各抓一次 `/serverinfo` XML，逐字相同
    void run_t1b(reporter_t &r, const request_t &req, const std::atomic<bool> &stop);

    /// T2：以 `vr_probe --mode ipcpeer` 驗 §F.5 T2 的情境（握手、消費、kill、RECONFIG、fence=UINT64_MAX、所有權、
    ///     不讀 pipe、雙寫入者、升權嘗試、停止 Signal、第二 instance、ABI 不同、HELLO 大小不符、冒名、恢復、限速）；
    ///     live 消費（display_vr_t 60 s，V3）；gpu-hold 以 NOT-RUN until=V4 明列
    void run_t2(reporter_t &r, const request_t &req, const std::atomic<bool> &stop);

    /// T4（M1b V4，只有 --manual-steamvr）：真的 SteamVR driver（操作手冊以使用者身分啟動 SteamVR）握手、HMD Activate、
    ///     HMD_PRESENTING、selftest consumer；--probe scene 時讀回角落位元圖案與 descriptor 的 renderPose 比對
    void run_t4(reporter_t &r, const request_t &req, const std::atomic<bool> &stop);

    /// T6 的平台部分：QPC 換算、§B.8 descriptor 驗證純函式、ABI 表與 vr_probe 逐字比對、vr_probe unit
    void run_t6(reporter_t &r, const request_t &req, const std::atomic<bool> &stop);
  }  // namespace platform

  /**
   * @brief T1b（M1b V3）：抓本機 `/serverinfo` 的 XML（HTTP 埠、未配對的回應）。
   * @details 放在平台中立檔：curl 的 header 會帶進 winsock2，必須比 <Windows.h> 先 include。
   * @param err 失敗時的說明（curl 代碼、HTTP 狀態）。
   */
  std::optional<std::string> fetch_serverinfo_xml(std::string &err);

  /**
   * @brief vr_probe 的 stdout 行過濾（sec-m14；不可信輸入）。
   * @details 只接受以 `[VIPLE-VR-PROBE] ` 開頭的行；去掉前綴後截到 240 bytes，控制字元、非 ASCII 與 `[`
   *          換成 `?`。前綴由 server 重新加上，不信任對方行內自帶的 tag。
   * @return 過濾後的內容（不含前綴）；不是 probe 行時 nullopt。
   */
  std::optional<std::string> sanitize_probe_line(std::string_view raw);

  /**
   * @brief 把過濾後的 probe 行拆成 `key=value`（以空白分隔；沒有 `=` 的字當作 tag，只取第一個）。
   */
  struct probe_kv_t {
    std::string tag;  ///< 第一個不含 `=` 的字（例如 `ipcpeer`、`unit`、`abi-row`）
    std::string sub;  ///< 第二個不含 `=` 的字（例如 `ipcpeer summary` 的 `summary`）
    std::vector<std::pair<std::string, std::string>> kv;

    std::optional<std::string> get(std::string_view key) const;
    std::optional<int64_t> get_int(std::string_view key) const;
  };

  probe_kv_t parse_probe_kv(std::string_view sanitized);
}  // namespace vr::selftest
