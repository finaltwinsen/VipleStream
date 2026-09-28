// driver_log.h - VipleStream §VR：driver 端（與 vr_probe ipcpeer 共用）的最小 log 模組。
//
// - 行內容不含 tag；由 sink 自己加前綴（driver：IVRDriverLog 加 "[VIPLE-VR-DRV] "；vr_probe：stdout 加
//   "[VIPLE-VR-PROBE] ipc "）。server 從 IPC log ring 轉印時也會重新加上 "[VIPLE-VR-DRV]"（§B.8 第 8 點）。
// - 全域狀態只有 POD 與 SRWLOCK_INIT（常數初始化），沒有會呼叫 Win32 的全域建構子（§E.2 driver_main 規範）。
// - 節流：所有 sink 合計每秒最多 20 行（突發 20），被丟掉的行數在下一行開頭以 "(suppressed N) " 標出。
// - 絕不記 handle 值、完整 GUID、SID（只能記 RID）或任何機密；呼叫端負責。
#pragma once

#include <cstdint>

namespace vrdrv::log {

  enum level_e : int {
    info = 0,
    warning = 1,
    error = 2,
  };

  // sink：本機輸出（IVRDriverLog 或 stdout）。line 已格式化、只含可列印 ASCII、不含換行。
  using sink_fn = void (*)(void *ctx, int level, const char *line);

  // 設定本機 sink；nullptr 表示關閉。可在任何時候呼叫（內部加鎖）。
  void set_sink(sink_fn fn, void *ctx);

  // 設定轉送（IPC log ring）。ipc_client 在有 generation 時把行推進 shm 的 log ring。
  // 轉送函式不可再呼叫 vrdrv::log（會遞迴）；ipc_client 的實作遵守這條。
  void set_forward(sink_fn fn, void *ctx);

  // printf 風格；超過 480 字元截斷。
  void write(int level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

  // 測試用：目前被節流丟掉、尚未回報的行數。
  uint32_t suppressed_pending();

  // 測試用：重設節流狀態（vr_probe unit 驗 20 行/s 用）。
  void reset_throttle_for_test();

}  // namespace vrdrv::log

#define VRDRV_LOG_INFO(...) ::vrdrv::log::write(::vrdrv::log::info, __VA_ARGS__)
#define VRDRV_LOG_WARN(...) ::vrdrv::log::write(::vrdrv::log::warning, __VA_ARGS__)
#define VRDRV_LOG_ERROR(...) ::vrdrv::log::write(::vrdrv::log::error, __VA_ARGS__)
