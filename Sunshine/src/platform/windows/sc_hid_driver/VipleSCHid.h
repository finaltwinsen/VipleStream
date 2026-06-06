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

// Inject a Steam Controller input report (gen-2: 54-byte report id 0x45).
// Returns non-zero on success (0 on failure).
int VipleSCHidWrite(VIPLE_SCHID_HANDLE h, const uint8_t* data, int len);

// Send a feature report to the virtual device (forwarded back to client).
// Used to relay host->controller commands (haptics etc.) back over the wire.
typedef void (*VipleSCHidFeatureCb)(const uint8_t* data, int len, void* ctx);
void VipleSCHidSetFeatureCb(VIPLE_SCHID_HANDLE h, VipleSCHidFeatureCb cb, void* ctx);

void VipleSCHidClose(VIPLE_SCHID_HANDLE h);

// Diagnostic: human-readable reason for the last VipleSCHidOpen() device-open
// attempt (enum count, per-Valve-interface CreateFileW error, etc.) — for logs.
const char* VipleSCHidLastDiag(void);

#ifdef __cplusplus
}
#endif
