/*
 * §SC-HID: VipleStream Steam Controller Virtual HID Device (UMDF2)
 *
 * Creates a synthetic USB HID device with Valve's VID 0x28DE / PID 0x1302
 * (gen-2 / 2025 "new" Steam Controller) so that Steam on the host recognises
 * it as a real Steam Controller and applies Steam Input configuration
 * (trackpads, gyro, touch, haptics). Its streamed vendor report uses id 0x45
 * (USB / BLE direct) or 0x42 (through the Puck wireless receiver) — same layout.
 *
 * Implementation: UMDF2 HID minidriver hosted by the in-box, already-signed
 * mshidumdf.sys reflector.  The DLL itself runs in user-mode; no kernel
 * signing is required — only admin rights for the initial installation.
 *
 * INSTALL (one-time, requires elevation) — handled by Install-VipleSCHid.ps1:
 *   1. pnputil /add-driver VipleSCHid.inf /install   (stage package into store)
 *   2. create a ROOT devnode with HWID "Viple\SteamController" via SetupAPI
 *      (DIF_REGISTERDEVICE + UpdateDriverForPlugAndPlayDevices) so the driver
 *      binds. This is what `devcon install` does internally; the script does it
 *      without devcon so no WDK tools are required on the host.
 *
 * USAGE (from Sunshine):
 *   VipleSCHidOpen()   — create/find the virtual device, return handle
 *   VipleSCHidWrite()  — inject a 64-byte Steam Controller input report
 *   VipleSCHidClose()  — release handle
 */

#pragma once
#ifdef __cplusplus
extern "C" {
#endif

// NOTE: deliberately do NOT include <windows.h> in this PUBLIC header. It is
// included by Sunshine's input.cpp, and <windows.h> leaks VK_* (and other) macros
// that collide with input.cpp's own identifiers (e.g. `constexpr auto VK_F1 = 0x70;`
// -> "expected unqualified-id before numeric constant"). The public API only needs
// int / void* / uint8_t; the implementation (VipleSCHid.cpp) includes <windows.h>.
#include <stdint.h>

// Opaque handle
typedef void* VIPLE_SCHID_HANDLE;

// Returns NULL on failure.
VIPLE_SCHID_HANDLE VipleSCHidOpen(void);

// §SC-HID Phase 2C — transparent feature proxy.
//
// Poll the driver for the next pending Steam feature op (GET_FEATURE(0x03)).
// Returns  1 if an event was popped (out params filled),
//          0 if the poll succeeded but the ring was empty,
//         -1 if HidD_GetFeature(0x03) itself failed (handle likely dead —
//            callers count consecutive -1s and close/re-open the handle).
//   *op       : SC_EVT_OP_GET(1) = Steam read → ask client to refresh from real SC
//               SC_EVT_OP_SET(2) = Steam sent a query → forward it to real SC
//   *reportId : HID report id Steam targeted (0x01)
//   *seq      : round-trip cookie
//   query/qlen: up to 59 query bytes Steam sent (op=SET; bytes 5..63 of the
//               0x03 report). query must be >=64 bytes.
//
// §SC-HID Round 1（driver 1.0.5.0）：0x03 是純事件報告
// （[03][op][reportId][seq][queryLen][query≤59]），driver 統計改走獨立的
// Feature 0x05（見 VipleSCHidReadDriverStats），不再佔用 0x03 的尾段。
int VipleSCHidPollNotify(VIPLE_SCHID_HANDLE h, uint8_t* op, uint8_t* reportId,
                         uint8_t* seq, uint8_t* query, int* qlen);

// §SC-HID WP2：process 級統計快照。所有計數在 VipleSCHid.cpp 內為 std::atomic，
// input 執行緒、poll 執行緒與 session 解構子可並行讀取。
// per-session 數字由呼叫端自行「快照相減」（input_t 在 poll 執行緒啟動時存一份
// baseline，10 s stats 與 final stats 印差值）。
typedef struct VipleSCHidStats {
    unsigned long writeOk;            // WriteFile 成功注入（含 keepalive 重放）
    unsigned long writeSkipped;       // 未宣告 report id 而略過（VipleSCHidWrite 回 2）
    unsigned long writeFailed;        // WriteFile 失敗（含 fatal）
    unsigned long lastWriteErr;       // 最後一次 WriteFile 的 GetLastError
    uint8_t       lastSkippedId;      // 最後一個被略過的 report id
    unsigned long notifyPolls;        // GET_FEATURE(0x03) 成功次數
    unsigned long notifyPollErrs;     // GET_FEATURE(0x03) 失敗次數
    unsigned long notifyEvents;       // 從 0x03 pop 到的 Steam feature 事件數
    unsigned long responsesDelivered; // VipleSCHidDeliverResponse 成功次數
} VipleSCHidStats;
void VipleSCHidGetStats(VipleSCHidStats* out);

// §SC-HID Round 1：driver 端統計（Feature 0x05，driver ≥1.0.5.0）。
// 由 HidD_GetFeature(0x05) 一次讀回 64 bytes，佈局（LE16 直接用，driver 端
// 計數器隨裝置 context 建立歸零、無需解迴繞）：
//   [0]=0x05 [1..2]=SET(0x01) 次數 [3..4]=GET(0x01) 次數 [5]=最後 0x01 feature
//   op 的 report id [6]=ring 深度 [7]=ResponsePending [8]=ResponseReady
//   [9..10]=GET 事件 push 數 [11..12]=有效回應交付數 [13..14]=GET 被閘控次數
//   [15..16]=全零回應被忽略數 [17..18]=driver 版本（0x05,0x01 → 1.5）
// 舊 driver（1.0.4.0）沒宣告 0x05 → HidD_GetFeature 失敗、函式回 0。
// 只由 poll 執行緒每 10 s 呼叫一次（不進 20 ms 熱路徑）。
typedef struct VipleSCHidDrvStats {
    uint16_t setCount;        // SET_FEATURE(0x01) ioctl 次數（Steam → driver）
    uint16_t getCount;        // GET_FEATURE(0x01) ioctl 次數
    uint8_t  lastReportId;    // 最後一個 0x01 feature op 的 report id
    uint8_t  ringDepth;       // 事件 ring 目前深度
    uint8_t  responsePending; // 1 = Steam 已 SET、client 回應尚未交付
    uint8_t  responseReady;   // 1 = 回應已交付、等 Steam 的下一次 GET 取走
    uint16_t getEvtPushed;    // 無 SET 在途時 push 的 GET 事件數（driver 端 500 ms 節流後）
    uint16_t delivered;       // WriteFile(0x04) 有效交付次數（全零者不計）
    uint16_t getGated;        // GET(0x01) 因閘控回失敗的次數
    uint16_t zeroDropped;     // WriteFile(0x04) 全零 payload 被忽略次數
    uint8_t  verMajor;        // [18]（LE16 高位元組；driver 1.0.5 → 1）
    uint8_t  verMinor;        // [17]（LE16 低位元組；driver 1.0.5 → 5）
} VipleSCHidDrvStats;
// Returns 1 and fills *out on success; 0 on failure (h null / GetFeature
// failed — *out is zeroed and *err (optional) receives GetLastError).
int VipleSCHidReadDriverStats(VIPLE_SCHID_HANDLE h, VipleSCHidDrvStats* out, unsigned long* err);

// Diagnostic: feature-proxy 側 process 級統計一行（featureEvts / responses /
// pollErrs），thread-local 緩衝，可與 VipleSCHidWriteDiag() 串成一行 10 s stats。
const char* VipleSCHidFeatureDiag(void);

// Deliver the real SC's feature response back into the driver via WriteFile
// (output report id 0x04) — used instead of HidD_SetFeature, which fails
// (result=0) on this UMDF reflector. `data` is the real SC's 64-byte feature
// response (data[0] is its report id); the driver stores data[1..63] as the
// reply Steam's GET_FEATURE(0x01) returns. Returns 1 on success, 0 on failure.
int VipleSCHidDeliverResponse(VIPLE_SCHID_HANDLE h, uint8_t seq,
                              const uint8_t* data, int len);

// Inject a Steam Controller input report (gen-2: 54-byte report id 0x45 =
// STATE_BLE, or 0x42 = ID_TRITON_CONTROLLER_STATE as emitted through the Puck
// receiver; both share the TritonMTUNoQuat_t layout and the virtual device
// declares both since driver 1.0.5.0).
// Returns:  1 = injected (WriteFile succeeded) — the only value that counts as
//               real input flow,
//           2 = skipped: the report id is not one the virtual device declares
//               (writing it would always be rejected). Counted as `skipped` in
//               VipleSCHidWriteDiag(); callers must NOT treat it as an
//               injection (no keepalive cache update, no "first inject" log),
//           0 = write failed but the handle is likely still usable (e.g.
//               HIDClass parameter rejection) — caller should KEEP the handle,
//          -1 = fatal (device gone: 1167/2/6/995/433/55) — caller should close
//               and re-open.
int VipleSCHidWrite(VIPLE_SCHID_HANDLE h, const uint8_t* data, int len);

// Diagnostic: short snapshot of write-path stats (written/skipped counts and
// the last WriteFile error code) — for rate-limited log lines. Thread-local
// buffer: safe to call from the input thread, the poll thread and the session
// destructor concurrently.
const char* VipleSCHidWriteDiag(void);

// Send a feature report to the virtual device (forwarded back to client).
// Used to relay host->controller commands (haptics etc.) back over the wire.
typedef void (*VipleSCHidFeatureCb)(const uint8_t* data, int len, void* ctx);
void VipleSCHidSetFeatureCb(VIPLE_SCHID_HANDLE h, VipleSCHidFeatureCb cb, void* ctx);

void VipleSCHidClose(VIPLE_SCHID_HANDLE h);

// Diagnostic: human-readable reason for the last VipleSCHidOpen() /
// VipleSCHidOpenDirect() device-open attempt (enum count, per-Valve-interface
// CreateFileW error, installer status, etc.) — for logs. Returns a snapshot
// copied into a thread-local buffer, so the input thread and the poll thread
// (both of which open handles) can each log their own attempt without
// tearing each other's string.
const char* VipleSCHidLastDiag(void);

// Reset the 5-second open throttle so the next VipleSCHidOpen() call runs
// immediately. Call after a forced device reconnect.
void VipleSCHidResetOpenThrottle(void);

// Open a direct handle bypassing the per-session throttle.
// Intended for the dedicated polling thread that manages its own handle.
VIPLE_SCHID_HANDLE VipleSCHidOpenDirect(void);

// Disable + re-enable the VipleSCHid root PDO via CM API so that Steam
// receives a DeviceRemoved + DeviceAdded event and re-runs GetControllerInfo.
// Should be called ~500 ms after session start, after the feature proxy is
// active. Returns 1 on success, 0 if the device node was not found.
int VipleSCHidForceReconnect(void);

#ifdef __cplusplus
}
#endif
