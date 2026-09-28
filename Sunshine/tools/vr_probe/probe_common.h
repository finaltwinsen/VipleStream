// probe_common.h - VipleStream §VR：vr_probe 的共用宣告（輸出、時間、參數）。
//
// vr_probe 不出貨（§F.4）：由 server 的 selftest 以主控台使用者的 token 啟動（stdout 經匿名 pipe 回 server），
// 或在開發機上手動執行。stdout 的每一行都是 "[VIPLE-VR-PROBE] <內容>"，內容只含可列印 ASCII、不含 '['，
// 長度 < 240（server 以 §F.4 的規則過濾並重新加前綴；不信任行內自帶的 tag）。
// stdout 寫入失敗（server 被殺、pipe 斷）一律忽略，繼續寫 CSV（ops-M12）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include <windows.h>

namespace probe {

  // ── 輸出 ─────────────────────────────────────────────────────────────────
  // printf 風格；自動加 "[VIPLE-VR-PROBE] " 前綴與換行，'[' 與控制字元換成 '?'，截到 239 字元。
  void line(const char *fmt, ...);

  // 印 ABI 表（vr_ipc_abi_table.h 的格式：abi-row <key>=<value> ×N，最後 abi-rows count=<n> digest=<16 hex>）
  void print_abi_table();

  // ── 時間 ─────────────────────────────────────────────────────────────────
  int64_t qpc();
  int64_t qpf();
  double qpc_to_ms(int64_t ticks);

  // ── 輸出目錄與 CSV ──────────────────────────────────────────────────────────
  // --out 沒給時用 %LOCALAPPDATA%\VipleStream\vr_probe\<utc>\（FOLDERID_LocalAppData，flags=0）。
  // 回傳是否成功；失敗時 CSV 不寫，但 stdout 照常。
  bool prepare_out_dir(const std::wstring &requested, std::wstring &out_dir);

  class csv_t {
  public:
    csv_t() = default;
    ~csv_t();
    csv_t(const csv_t &) = delete;
    csv_t &operator=(const csv_t &) = delete;

    bool open(const std::wstring &dir, const wchar_t *name, const char *header);
    void row(const char *fmt, ...);
    void close();

  private:
    FILE *f_ = nullptr;
  };

  // ── 參數 ─────────────────────────────────────────────────────────────────
  struct args_t {
    std::string mode;
    std::wstring out;
    double seconds = 10.0;

    // ipcpeer（§F.4、§F.5 T2）
    std::wstring pipe;  // 空 → 預設 \\.\pipe\VipleStreamVR-<自己的 session id>
    uint32_t abi = 0;  // 0 = 不覆寫（VRIPC_ABI_VERSION）
    uint32_t hello_size = 0;  // 0 = 不覆寫（sizeof(vripc_hello_t)）
    uint32_t stop_signal_after = 0;  // --peer-stop-signal-after N
    uint32_t crash_after = 0;  // --peer-crash-after N
    bool never_read = false;  // --peer-never-read
    bool close_after_welcome = false;  // --peer-close-after-welcome
    bool fence_max = false;  // --peer-fence-max
    uint32_t fence_max_after = 30;  // --peer-fence-max-after N（預設第 30 幀之後）
    bool escalate = false;  // --peer-escalate
    uint32_t gpu_hold_ms = 0;  // --peer-gpu-hold-ms X
    uint32_t writers = 1;  // --peer-writers N（只記錄；tracking 統計一律都做）
    bool no_flush = false;  // --peer-no-flush（E-3：Signal 後不 Flush）
    uint32_t hello_repeat = 0;  // --hello-repeat N（限速測試：連續 N 次連線嘗試）
    uint32_t hello_interval_ms = 50;  // --hello-interval-ms M
    bool no_render = false;  // --peer-no-render：握手後不發布幀（只測握手／tracking）

    // unit
    bool no_loopback = false;  // --no-loopback：只跑純函式測試
  };

  int run_ipcpeer(const args_t &a);
  int run_unit(const args_t &a);

  // 回傳碼（selftest 依此判讀；情境的預期結果以 stdout 的 key=value 為準）
  constexpr int rc_ok = 0;  // 跑完（不代表情境「成功」，見 summary 行）
  constexpr int rc_failed = 1;  // 內部錯誤或 unit 有失敗
  constexpr int rc_usage = 2;  // 參數錯誤
  constexpr int rc_not_implemented = 3;  // 這個模式要到 V4 才有

}  // namespace probe
