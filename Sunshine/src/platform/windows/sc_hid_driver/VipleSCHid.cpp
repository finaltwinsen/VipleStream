/*
 * §SC-HID: VipleSCHid.cpp — User-mode client for the UMDF2 virtual HID device
 *
 * After the driver is installed (pnputil /add-driver + devcon install), this
 * code opens the device's HID interface via CreateFile, writes 64-byte input
 * reports, and receives feature/output reports via a completion-port thread.
 */

#include "VipleSCHid.h"
#include <hidsdi.h>
#include <setupapi.h>
#include <devguid.h>
#include <initguid.h>
#include <hidclass.h>
#include <shellapi.h>
#include <shlobj.h>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shell32.lib")

// Valve Steam Controller USB identifiers
static constexpr USHORT SC_VID = 0x28DE;
static constexpr USHORT SC_PID = 0x1102;

struct VipleSCHidCtx {
    HANDLE hDev;
    VipleSCHidFeatureCb featureCb;
    void* featureCtx;
};

// Enumerate HID devices and return the first that matches VID/PID.
static HANDLE openHidByVidPid(USHORT vid, USHORT pid) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    SP_DEVICE_INTERFACE_DATA ifaceData = {};
    ifaceData.cbSize = sizeof(ifaceData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifaceData); ++i) {
        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetail(devInfo, &ifaceData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) continue;

        auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            HeapAlloc(GetProcessHeap(), 0, requiredSize));
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail, requiredSize, nullptr, nullptr)) {
            HANDLE h = CreateFileW(detail->DevicePath,
                GENERIC_READ | GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

            if (h != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attrs = {};
                attrs.Size = sizeof(attrs);
                if (HidD_GetAttributes(h, &attrs) &&
                    attrs.VendorID == vid && attrs.ProductID == pid) {
                    HeapFree(GetProcessHeap(), 0, detail);
                    SetupDiDestroyDeviceInfoList(devInfo);
                    return h;
                }
                CloseHandle(h);
            }
        }
        HeapFree(GetProcessHeap(), 0, detail);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return INVALID_HANDLE_VALUE;
}

// Try to install the UMDF2 driver if not already present.
// Expects Install-VipleSCHid.ps1 in the same directory as the binary.
static void tryInstallDriver(void) {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) return;
    *(lastSlash + 1) = L'\0';

    wchar_t scriptPath[MAX_PATH] = {};
    _snwprintf_s(scriptPath, MAX_PATH, _TRUNCATE, L"%ssc_hid_driver\\Install-VipleSCHid.ps1", exePath);

    if (GetFileAttributesW(scriptPath) == INVALID_FILE_ATTRIBUTES) return;

    wchar_t params[MAX_PATH * 2] = {};
    _snwprintf_s(params, _countof(params), _TRUNCATE,
        L"-ExecutionPolicy Bypass -NonInteractive -File \"%s\"", scriptPath);

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
    HANDLE hDev = openHidByVidPid(SC_VID, SC_PID);
    if (hDev == INVALID_HANDLE_VALUE) {
        // Driver not installed — try auto-install and retry once
        tryInstallDriver();
        // Give Windows PnP a moment to enumerate the new device
        Sleep(2000);
        hDev = openHidByVidPid(SC_VID, SC_PID);
    }
    if (hDev == INVALID_HANDLE_VALUE) return nullptr;

    auto* ctx = new VipleSCHidCtx{hDev, nullptr, nullptr};
    return ctx;
}

BOOL VipleSCHidWrite(VIPLE_SCHID_HANDLE h, const uint8_t* data, int len) {
    if (!h || len != 64) return FALSE;
    auto* ctx = static_cast<VipleSCHidCtx*>(h);

    // HID write: prepend 1-byte report ID 0 (no report IDs in raw mode)
    uint8_t buf[65] = {};
    buf[0] = 0;
    memcpy(buf + 1, data, 64);

    DWORD written = 0;
    return WriteFile(ctx->hDev, buf, 65, &written, nullptr);
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
