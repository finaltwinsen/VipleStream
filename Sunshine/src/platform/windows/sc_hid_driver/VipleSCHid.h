/*
 * §SC-HID: VipleStream Steam Controller Virtual HID Device (UMDF2)
 *
 * Creates a synthetic USB HID device with Valve's VID 0x28DE / PID 0x1302
 * (gen-2 / 2025 "new" Steam Controller) so that Steam on the host recognises
 * it as a real Steam Controller and applies Steam Input configuration
 * (trackpads, gyro, touch, haptics). Its streamed vendor report uses id 0x45.
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
// Returns 1 if an event was popped (out params filled), 0 if none / error.
//   *op       : SC_EVT_OP_GET(1) = Steam read → ask client to refresh from real SC
//               SC_EVT_OP_SET(2) = Steam sent a query → forward it to real SC
//   *reportId : HID report id Steam targeted (0x01)
//   *seq      : round-trip cookie
//   query/qlen: up to 59 query bytes Steam sent (op=SET). query must be >=64 bytes.
int VipleSCHidPollNotify(VIPLE_SCHID_HANDLE h, uint8_t* op, uint8_t* reportId,
                         uint8_t* seq, uint8_t* query, int* qlen);

// Deliver the real SC's feature response back into the driver via WriteFile
// (output report id 0x04) — used instead of HidD_SetFeature, which fails
// (result=0) on this UMDF reflector. `data` is the real SC's 64-byte feature
// response (data[0] is its report id); the driver stores data[1..63] as the
// reply Steam's GET_FEATURE(0x01) returns. Returns 1 on success, 0 on failure.
int VipleSCHidDeliverResponse(VIPLE_SCHID_HANDLE h, uint8_t seq,
                              const uint8_t* data, int len);

// Inject a Steam Controller input report (gen-2: 54-byte report id 0x45).
// Returns:  1 = injected (or skipped: a non-0x45 report the virtual device
//               does not declare — writing those would always be rejected),
//           0 = write failed but the handle is likely still usable (e.g.
//               HIDClass parameter rejection) — caller should KEEP the handle,
//          -1 = fatal (device gone) — caller should close and re-open.
int VipleSCHidWrite(VIPLE_SCHID_HANDLE h, const uint8_t* data, int len);

// Diagnostic: short snapshot of write-path stats (written/skipped counts and
// the last WriteFile error code) — for rate-limited log lines.
const char* VipleSCHidWriteDiag(void);

// Send a feature report to the virtual device (forwarded back to client).
// Used to relay host->controller commands (haptics etc.) back over the wire.
typedef void (*VipleSCHidFeatureCb)(const uint8_t* data, int len, void* ctx);
void VipleSCHidSetFeatureCb(VIPLE_SCHID_HANDLE h, VipleSCHidFeatureCb cb, void* ctx);

void VipleSCHidClose(VIPLE_SCHID_HANDLE h);

// Diagnostic: human-readable reason for the last VipleSCHidOpen() device-open
// attempt (enum count, per-Valve-interface CreateFileW error, etc.) — for logs.
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
