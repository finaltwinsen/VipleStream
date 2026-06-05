/*
 * §SC-HID: VipleStream Steam Controller Virtual HID Device (UMDF2)
 *
 * Creates a synthetic USB HID device with Valve's VID 0x28DE / PID 0x1102
 * so that Steam on the host recognises it as a real Steam Controller and
 * applies Steam Input configuration (trackpads, gyro, haptics).
 *
 * Implementation: UMDF2 HID minidriver hosted by the in-box, already-signed
 * mshidumdf.sys reflector.  The DLL itself runs in user-mode; no kernel
 * signing is required — only admin rights for the initial installation.
 *
 * INSTALL (one-time, requires elevation):
 *   pnputil /add-driver VipleSCHid.inf /install
 *   devcon install VipleSCHid.inf Viple\SteamController
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

#include <windows.h>
#include <stdint.h>

// Opaque handle
typedef void* VIPLE_SCHID_HANDLE;

// Returns NULL on failure.
VIPLE_SCHID_HANDLE VipleSCHidOpen(void);

// Inject a 64-byte Steam Controller input report.
// Returns TRUE on success.
BOOL VipleSCHidWrite(VIPLE_SCHID_HANDLE h, const uint8_t* data, int len);

// Send a feature report to the virtual device (forwarded back to client).
// Used to relay host->controller commands (haptics etc.) back over the wire.
typedef void (*VipleSCHidFeatureCb)(const uint8_t* data, int len, void* ctx);
void VipleSCHidSetFeatureCb(VIPLE_SCHID_HANDLE h, VipleSCHidFeatureCb cb, void* ctx);

void VipleSCHidClose(VIPLE_SCHID_HANDLE h);

#ifdef __cplusplus
}
#endif
