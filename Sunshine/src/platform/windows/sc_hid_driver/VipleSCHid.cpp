/*
 * §SC-HID: VipleSCHid.cpp — User-mode client for the UMDF2 virtual HID device
 *
 * After the driver is installed (Install-VipleSCHid.ps1: pnputil /add-driver +
 * SetupAPI root-devnode creation), this code opens the device's HID interface
 * via CreateFile, writes 64-byte input reports, and receives feature/output
 * reports via a completion-port thread.
 */

#include "VipleSCHid.h"
#include <windows.h>   // implementation needs the full Win32 API (HANDLE, CreateFileW, ...)
#include <hidsdi.h>
#include <setupapi.h>
#include <cfgmgr32.h>  // CM_Get_Device_ID_List — device-instance-anchored fallback open
#include <devguid.h>
#include <initguid.h>
#include <hidclass.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>   // StringCchPrintfW — portable safe printf (MSVC + MinGW)
#include <cstdio>      // vsnprintf (server-open diagnostic)
#include <cstdarg>     // va_list
#include <cstring>     // strlen, wcsstr

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "shell32.lib")

// gen-2 Steam Controller — our virtual device presents 0x28DE/0x1302.
// (On the host there is no real SC, so matching 0x1302 finds our virtual device.)
static constexpr USHORT SC_VID = 0x28DE;
static constexpr USHORT SC_PID = 0x1302;

struct VipleSCHidCtx {
    HANDLE hDev;
    VipleSCHidFeatureCb featureCb;
    void* featureCtx;
};

// §SC-HID server-open diagnostic: openHidByVidPid records the FULL probe trail
// (every interface path, every error code, both strategies) so a failing host
// can be diagnosed from sunshine.log via VipleSCHidLastDiag() instead of
// guessing. Lesson learned: the previous diag matched "valve" interfaces by the
// path substring "vid_28de" — but our virtual device is ROOT-enumerated, so its
// HIDClass child PDO path need not contain vid_xxxx at all. An open failure on
// it was silently skipped and got misdiagnosed as "SYSTEM cannot enumerate it".
static char g_diag[4096] = {};
static void diagAppend(const char* fmt, ...) {
    size_t len = strlen(g_diag);
    if (len >= sizeof(g_diag) - 96) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(g_diag + len, sizeof(g_diag) - len, fmt, ap);
    va_end(ap);
}

// Append a trimmed interface path: drop the "\\?\" prefix and the trailing
// "#{interface-class-guid}" (identical for every HID interface) to save diag
// space — what identifies the device is the middle hid#...#instance part.
static void diagAppendPath(const wchar_t* path) {
    const wchar_t* p = path;
    if (wcsncmp(p, L"\\\\?\\", 4) == 0) p += 4;
    const wchar_t* end = wcsstr(p, L"#{");
    int n = end ? (int)(end - p) : (int)wcslen(p);
    if (n > 96) n = 96;
    diagAppend("%.*ls", n, p);
}

// Probe one HID interface path. The probe opens with desiredAccess=0 — that
// succeeds even on devices whose read/write access is denied to this caller
// (same trick hidapi uses to enumerate keyboards) and still allows
// HidD_GetAttributes, so "access denied" and "wrong device" become
// distinguishable in the diag. On a VID/PID match, reopen with the access
// VipleSCHidWrite() needs. Synchronous handle on purpose: VipleSCHidWrite uses
// plain WriteFile with no OVERLAPPED, which is undefined on a handle opened
// with FILE_FLAG_OVERLAPPED (the old code did exactly that).
static HANDLE probeAndOpen(const wchar_t* path, USHORT vid, USHORT pid) {
    diagAppend(" [");
    diagAppendPath(path);

    HANDLE h0 = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (h0 == INVALID_HANDLE_VALUE) {
        diagAppend(" probe0Err=%lu]", (unsigned long)GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    HIDD_ATTRIBUTES attrs = {};
    attrs.Size = sizeof(attrs);
    bool gotA = HidD_GetAttributes(h0, &attrs);
    DWORD attrErr = gotA ? 0 : GetLastError();
    CloseHandle(h0);
    if (!gotA) {
        diagAppend(" attrErr=%lu]", (unsigned long)attrErr);
        return INVALID_HANDLE_VALUE;
    }
    diagAppend(" %04X:%04X", attrs.VendorID, attrs.ProductID);
    if (attrs.VendorID != vid || attrs.ProductID != pid) {
        diagAppend("]");
        return INVALID_HANDLE_VALUE;
    }

    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        diagAppend(" MATCH openRW]");
        return h;
    }
    DWORD rwErr = GetLastError();
    // Injection only needs write access — degrade gracefully if read is denied.
    h = CreateFileW(path, GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        diagAppend(" MATCH openW(rwErr=%lu)]", (unsigned long)rwErr);
        return h;
    }
    diagAppend(" MATCH rwErr=%lu wErr=%lu]", (unsigned long)rwErr, (unsigned long)GetLastError());
    return INVALID_HANDLE_VALUE;
}

// Strategy 1: classic device-interface-class enumeration (what hidapi/Steam do).
static HANDLE openViaSetupDi(const GUID* hidGuid, USHORT vid, USHORT pid) {
    HDEVINFO devInfo = SetupDiGetClassDevs(hidGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) {
        diagAppend(" SetupDiGetClassDevs err=%lu", (unsigned long)GetLastError());
        return INVALID_HANDLE_VALUE;
    }

    HANDLE found = INVALID_HANDLE_VALUE;
    int total = 0;
    for (DWORD i = 0; found == INVALID_HANDLE_VALUE; ++i) {
        SP_DEVICE_INTERFACE_DATA ifaceData = {};
        ifaceData.cbSize = sizeof(ifaceData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, hidGuid, i, &ifaceData)) {
            // A premature stop (anything but NO_MORE_ITEMS) would itself explain
            // a "device not found" — record it instead of silently breaking.
            DWORD e = GetLastError();
            if (e != ERROR_NO_MORE_ITEMS)
                diagAppend(" enumStop@%lu err=%lu", (unsigned long)i, (unsigned long)e);
            break;
        }
        total++;

        // Explicit W: Sunshine builds WITHOUT the UNICODE macro, so the generic
        // SetupDiGetDeviceInterfaceDetail resolves to the ANSI version. Its
        // (smaller) requiredSize then made the W detail call below fail with
        // ERROR_INSUFFICIENT_BUFFER for EVERY interface — silently skipping all
        // of them. That single A/W mismatch was the entire "server cannot find
        // the virtual device" failure; account/session/ACL never mattered.
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) continue;

        auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            HeapAlloc(GetProcessHeap(), 0, requiredSize));
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail, requiredSize, nullptr, nullptr))
            found = probeAndOpen(detail->DevicePath, vid, pid);

        HeapFree(GetProcessHeap(), 0, detail);
    }
    diagAppend(" (setupdi ifaces=%d)", total);
    SetupDiDestroyDeviceInfoList(devInfo);
    return found;
}

// Strategy 2 (fallback): anchor on device INSTANCES instead of the interface
// class. CM_Get_Device_ID_List(enumerator="HID") lists every HIDClass child
// PDO directly from the PnP tree, then CM_Get_Device_Interface_List maps each
// instance to its interface path. If the class-interface enumeration above
// behaves differently for this caller (the SYSTEM-service mystery), this path
// both works around it and logs the full HID instance list for diagnosis.
static HANDLE openViaCfgMgr(const GUID* hidGuid, USHORT vid, USHORT pid) {
    ULONG sz = 0;
    CONFIGRET cr = CM_Get_Device_ID_List_SizeW(&sz, L"HID",
        CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT);
    if (cr != CR_SUCCESS || sz == 0) {
        diagAppend(" cmIdListSize cr=%lu", (unsigned long)cr);
        return INVALID_HANDLE_VALUE;
    }
    auto* ids = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, sz * sizeof(wchar_t)));
    if (!ids) return INVALID_HANDLE_VALUE;
    cr = CM_Get_Device_ID_ListW(L"HID", ids, sz,
        CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT);
    if (cr != CR_SUCCESS) {
        diagAppend(" cmIdList cr=%lu", (unsigned long)cr);
        HeapFree(GetProcessHeap(), 0, ids);
        return INVALID_HANDLE_VALUE;
    }

    HANDLE found = INVALID_HANDLE_VALUE;
    for (wchar_t* id = ids; *id && found == INVALID_HANDLE_VALUE; id += wcslen(id) + 1) {
        diagAppend(" {%.64ls", id);
        ULONG isz = 0;
        cr = CM_Get_Device_Interface_List_SizeW(&isz, const_cast<GUID*>(hidGuid), id,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr != CR_SUCCESS || isz <= 1) {
            diagAppend(" noIf cr=%lu}", (unsigned long)cr);
            continue;
        }
        auto* ifs = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0, isz * sizeof(wchar_t)));
        if (!ifs) { diagAppend("}"); continue; }
        cr = CM_Get_Device_Interface_ListW(const_cast<GUID*>(hidGuid), id, ifs, isz,
            CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_SUCCESS) {
            for (wchar_t* p = ifs; *p && found == INVALID_HANDLE_VALUE; p += wcslen(p) + 1)
                found = probeAndOpen(p, vid, pid);
        }
        HeapFree(GetProcessHeap(), 0, ifs);
        diagAppend("}");
    }
    HeapFree(GetProcessHeap(), 0, ids);
    return found;
}

// Find + open the virtual SC by VID/PID, trying both strategies.
static HANDLE openHidByVidPid(USHORT vid, USHORT pid) {
    g_diag[0] = '\0';

    // One-line caller identity. The SYSTEM-vs-interactive-user and
    // restricted-token hypotheses keep coming up for this open path — settle
    // them from the log of the failing process itself.
    DWORD sess = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sess);
    HANDLE tok = nullptr;
    BOOL restricted = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        restricted = IsTokenRestricted(tok);
        CloseHandle(tok);
    }
    diagAppend("sess=%lu restr=%d", (unsigned long)sess, restricted ? 1 : 0);

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HANDLE h = openViaSetupDi(&hidGuid, vid, pid);
    if (h == INVALID_HANDLE_VALUE) {
        diagAppend(" | cfgmgr:");
        h = openViaCfgMgr(&hidGuid, vid, pid);
    }
    if (h == INVALID_HANDLE_VALUE)
        diagAppend(" => %04X:%04X NOT opened", vid, pid);
    return h;
}

// Returns a human-readable reason for the last openHidByVidPid result (for logs).
const char* VipleSCHidLastDiag(void) { return g_diag; }

// Try to install the UMDF2 driver if not already present.
// Expects Install-VipleSCHid.ps1 in the same directory as the binary.
static void tryInstallDriver(void) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) return;
    *(lastSlash + 1) = L'\0';

    // StringCchPrintfW (strsafe.h) rather than MSVC-only _snwprintf_s or the
    // MinGW-gated swprintf — this file compiles under both MSVC and MinGW/UCRT.
    wchar_t scriptPath[MAX_PATH] = {};
    StringCchPrintfW(scriptPath, MAX_PATH, L"%lssc_hid_driver\\Install-VipleSCHid.ps1", exePath);

    if (GetFileAttributesW(scriptPath) == INVALID_FILE_ATTRIBUTES) return;

    wchar_t params[MAX_PATH * 2] = {};
    StringCchPrintfW(params, MAX_PATH * 2,
        L"-ExecutionPolicy Bypass -NonInteractive -File \"%ls\"", scriptPath);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb       = L"runas";
    sei.lpFile       = L"powershell.exe";
    sei.lpParameters = params;
    sei.nShow        = SW_HIDE;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        CloseHandle(sei.hProcess);
    }
}

VIPLE_SCHID_HANDLE VipleSCHidOpen(void) {
    // The caller lazy-opens on EVERY incoming report while the device stays
    // unopenable — without a throttle that is a full enumeration + probe storm
    // at input-report rate (and the old code even re-ran the installer +
    // Sleep(2000) per report, which is where the "retry every 2s" cadence in
    // the logs came from). Real attempts at most every 5 s; throttled calls
    // leave the diag empty so the caller knows not to log them.
    static ULONGLONG lastAttempt = 0;
    ULONGLONG now = GetTickCount64();
    if (lastAttempt != 0 && now - lastAttempt < 5000) {
        g_diag[0] = '\0';
        return nullptr;
    }
    lastAttempt = now;

    HANDLE hDev = openHidByVidPid(SC_VID, SC_PID);
    if (hDev == INVALID_HANDLE_VALUE) {
        // Driver not installed — try auto-install (once per process) and retry
        static bool installTried = false;
        if (!installTried) {
            installTried = true;
            tryInstallDriver();
            // Give Windows PnP a moment to enumerate the new device
            Sleep(2000);
            hDev = openHidByVidPid(SC_VID, SC_PID);
        }
    }
    if (hDev == INVALID_HANDLE_VALUE) return nullptr;

    auto* ctx = new VipleSCHidCtx{hDev, nullptr, nullptr};
    return ctx;
}

// Write-path stats for rate-limited logging (single input thread — no locking).
static unsigned long g_writeOk = 0, g_writeSkipped = 0, g_writeFailed = 0, g_lastWriteErr = 0;
static uint8_t g_lastSkippedId = 0;
static char g_writeDiag[128] = {};

const char* VipleSCHidWriteDiag(void) {
    // vsnprintf-free: tiny fixed format, built on demand
    snprintf(g_writeDiag, sizeof(g_writeDiag),
             "ok=%lu skipped=%lu(lastId=0x%02X) failed=%lu lastErr=%lu",
             g_writeOk, g_writeSkipped, g_lastSkippedId, g_writeFailed, g_lastWriteErr);
    return g_writeDiag;
}

int VipleSCHidWrite(VIPLE_SCHID_HANDLE h, const uint8_t* data, int len) {
    if (!h || len <= 0) return 0;
    auto* ctx = static_cast<VipleSCHidCtx*>(h);

    // The virtual device's descriptor declares OUTPUT report id 0x45 only. The
    // client forwards EVERY vendor interface of the physical SC + Puck (5 of
    // them), so reports with other ids (Puck management traffic etc.) do come
    // through — HIDClass would reject those writes with ERROR_INVALID_PARAMETER
    // every time, and treating that as "device broke" caused a close/re-open
    // loop that dropped all input. Skip them instead.
    if (data[0] != 0x45) {
        g_writeSkipped++;
        g_lastSkippedId = data[0];
        return 1;
    }

    // gen-2 SC: the forwarded wire payload already begins with the HID report id
    // (0x45) at byte 0 — it IS a report-id'd report, so we do NOT prepend a 0
    // (that was the old 0x1102 report-id-0 convention). Write it as a 64-byte
    // OUTPUT report (OutputReportByteLength=64); the driver takes the leading
    // 54 bytes ([0x45]+53 data) to complete the pending INPUT read.
    uint8_t buf[64] = {};
    int n = (len > 64) ? 64 : len;
    memcpy(buf, data, n);

    DWORD written = 0;
    if (WriteFile(ctx->hDev, buf, sizeof(buf), &written, nullptr)) {
        g_writeOk++;
        return 1;
    }

    DWORD e = GetLastError();
    g_writeFailed++;
    g_lastWriteErr = e;
    // Only treat "the device object is gone" class errors as fatal; parameter
    // rejections and transient errors keep the (still valid) handle.
    switch (e) {
        case ERROR_DEVICE_NOT_CONNECTED:  // 1167
        case ERROR_FILE_NOT_FOUND:        // 2
        case ERROR_INVALID_HANDLE:        // 6
        case ERROR_OPERATION_ABORTED:     // 995 (device removal cancels I/O)
            return -1;
        default:
            return 0;
    }
}

int VipleSCHidSetFeature(VIPLE_SCHID_HANDLE h, const uint8_t* data, int len) {
    if (!h || len <= 0) return 0;
    auto* ctx = static_cast<VipleSCHidCtx*>(h);
    uint8_t buf[64] = {};
    int n = (len > 64) ? 64 : len;
    memcpy(buf, data, n);
    // HidD_SetFeature: buf[0]=report ID, rest=data; total=64 (report length incl. ID)
    return HidD_SetFeature(ctx->hDev, buf, sizeof(buf)) ? 1 : 0;
}

void VipleSCHidSetFeatureCb(VIPLE_SCHID_HANDLE h, VipleSCHidFeatureCb cb, void* ctx_) {
    if (!h) return;
    auto* ctx = static_cast<VipleSCHidCtx*>(h);
    ctx->featureCb = cb;
    ctx->featureCtx = ctx_;
}

void VipleSCHidClose(VIPLE_SCHID_HANDLE h) {
    if (!h) return;
    auto* ctx = static_cast<VipleSCHidCtx*>(h);
    CloseHandle(ctx->hDev);
    delete ctx;
}
