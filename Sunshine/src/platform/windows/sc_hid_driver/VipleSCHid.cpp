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
#include <objbase.h>   // CoInitializeEx — ShellExecuteEx 建議在呼叫執行緒先初始化 COM
#include <strsafe.h>   // StringCchPrintfW — portable safe printf (MSVC + MinGW)
#include <cstdio>      // vsnprintf (server-open diagnostic)
#include <cstdarg>     // va_list
#include <cstring>     // strlen, wcsstr
#include <atomic>      // §SC-HID WP2：寫入/輪詢統計跨 input / poll 執行緒共用
#include <mutex>       // §SC-HID WP2.4：安裝執行緒 ↔ VipleSCHidOpen 交換安裝結果
#include <string>      // std::wstring（安裝執行緒帶走 installer 路徑）
#include <thread>      // §SC-HID WP2.4：一次性 detached 安裝執行緒

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
//
// §SC-HID WP2（審查修正）：thread_local。開裝置的有兩條執行緒——input 執行緒
// （VipleSCHidOpen，lazy-open）與 poll 執行緒（VipleSCHidOpenDirect，reopen
// 退避）——兩者可能同時探測；共用一個 g_diag 會讓一方清掉另一方尚未印出的
// 軌跡、或在 log 行上讀到撕裂的字串。每條執行緒各自一份，探測與印出都在同
// 一條執行緒上，天然一致。
static thread_local char g_diag[4096] = {};
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
// §SC-HID WP2（審查修正）：回傳前複製進另一個 thread_local 快照緩衝——呼叫端
// 可能先取指標、再做另一次 open（會重置 g_diag）、最後才印；快照讓「取到的
// 字串」固定為取用當下的內容。
const char* VipleSCHidLastDiag(void) {
    static thread_local char snapshot[sizeof(g_diag)];
    size_t n = strlen(g_diag);
    if (n >= sizeof(snapshot)) n = sizeof(snapshot) - 1;
    memcpy(snapshot, g_diag, n);
    snapshot[n] = '\0';
    return snapshot;
}

// §SC-HID WP2.4：自動安裝的結果另存一份，因為 VipleSCHidOpen 在安裝後會再跑
// openHidByVidPid（它會清掉 g_diag），安裝結果得在那之後再 append 回 diag，
// 才會出現在 "device not found: …" / "device opened: …" 的 log 行上。
//
// §SC-HID WP2.4（審查修正 major）：安裝程序改在一次性的 detached std::thread
// 上執行。舊碼在 VipleSCHidOpen 內同步 ShellExecuteEx + WaitForSingleObject(30 s)
// + Sleep(2000)——而 VipleSCHidOpen 是從 input 封包處理路徑（task_pool 執行緒）
// 呼叫的，等於把整個 input pipeline 卡住最長 32 s。現在：
//   - 主執行緒只解析 installer 路徑並啟動執行緒，立刻回傳（diag 只 append
//     "installer launched async"）；
//   - 安裝執行緒等最多 60 s，把結果寫進 g_installDiag（installMutex 保護）並
//     設 g_installDone；
//   - 之後某次 VipleSCHidOpen 看到 g_installDone 就把結果 append 進那次的
//     diag（只回報一次，避免每 5 s 的 not-found 行都重印）。
//   - lpVerb=nullptr（不 runas）：service 以 SYSTEM 執行本來就有權限（已提權的
//     caller 用 runas 也不會彈 UI，直接成功）；改 nullptr 是為了避免「非提權互動式
//     開發環境」彈 UAC 同意視窗把 detached 執行緒卡住——該環境下 script 會自己
//     失敗、結束碼回報在 diag，請手動以管理員執行 Install-VipleSCHid.ps1。
// mutex 以 new 配置、永不解構：安裝執行緒是 detached，process 退出時若還在跑，
// 不能碰已被靜態解構的 mutex。
static std::mutex& installMutex() {
    static std::mutex* m = new std::mutex;
    return *m;
}
static char g_installDiag[512] = {};               // 受 installMutex() 保護
static std::atomic<bool> g_installLaunched{false}; // 本 process 只嘗試一次
static std::atomic<bool> g_installDone{false};     // 結果已寫入 g_installDiag
static std::atomic<bool> g_installReported{false}; // 結果已 append 進某次 open 的 diag

static void setInstallDiag(const char* text) {
    std::lock_guard<std::mutex> lk(installMutex());
    snprintf(g_installDiag, sizeof(g_installDiag), "%s", text);
}

// 安裝執行緒本體：啟動 powershell 跑 Install-VipleSCHid.ps1、等最多 60 s、寫結果。
// 逾時不 TerminateProcess——pnputil 跑到一半被殺可能留下半套 driver store 狀態，
// 比多一個背景 powershell 更糟；逾時只回報 timeout60s。
static void installDriverWorker(std::wstring scriptPath) {
    wchar_t params[MAX_PATH * 2] = {};
    StringCchPrintfW(params, MAX_PATH * 2,
        L"-ExecutionPolicy Bypass -NonInteractive -File \"%ls\"", scriptPath.c_str());

    // ShellExecuteEx 可能委派給 COM shell extension，MS 文件建議先初始化 COM；
    // 這是我們自己的執行緒，apartment 由我們決定。
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    char result[512] = {};
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb       = nullptr;   // 不 runas（見上方說明）
    sei.lpFile       = L"powershell.exe";
    sei.lpParameters = params;
    sei.nShow        = SW_HIDE;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        DWORD wait = WaitForSingleObject(sei.hProcess, 60000);
        DWORD rc = (DWORD)-1;
        if (wait == WAIT_OBJECT_0) GetExitCodeProcess(sei.hProcess, &rc);
        CloseHandle(sei.hProcess);
        snprintf(result, sizeof(result),
                 "installer=%ls wait=%s rc=%lu", scriptPath.c_str(),
                 wait == WAIT_OBJECT_0 ? "done" : (wait == WAIT_TIMEOUT ? "timeout60s" : "waitErr"),
                 (unsigned long)rc);
    }
    else {
        DWORD e = GetLastError();
        snprintf(result, sizeof(result),
                 "installer=%ls launch err=%lu", scriptPath.c_str(), (unsigned long)e);
    }
    if (SUCCEEDED(coInit)) CoUninitialize();

    setInstallDiag(result);
    g_installDone.store(true, std::memory_order_release);
}

// Try to install the UMDF2 driver if not already present (asynchronously).
// §SC-HID WP2.4（2026-09-02）：host 的 deploy 把 driver 4 檔（含
// Install-VipleSCHid.ps1）複製到 exe 根層，但舊碼只找 <exe>\sc_hid_driver\ 子
// 目錄（host 上不存在）→ 自動安裝從未觸發、也沒留下任何痕跡（F7）。改為先找
// 根層、再退回子目錄（開發機 build tree 佈局）。不加 -Reinstall（升級由 deploy
// 鏈負責）。
// 回傳：要立刻 append 進呼叫端 diag 的一小段狀態字串（thread_local 緩衝）。
// 找不到 installer / 開不出執行緒 → 結果直接寫進 g_installDiag 並標記完成
// （這種情況沒有非同步結果可等）。
static const char* tryInstallDriverAsync(void) {
    static thread_local char status[MAX_PATH * 2 + 96];
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* lastSlash = wcsrchr(exePath, L'\\');
    if (!lastSlash) {
        // 同步失敗：狀態由呼叫端當場 append，標記 reported 避免同一行再印一次。
        setInstallDiag("installer: exe dir unresolved");
        g_installReported.store(true, std::memory_order_relaxed);
        g_installDone.store(true, std::memory_order_release);
        return "installer: exe dir unresolved";
    }
    *(lastSlash + 1) = L'\0';

    // StringCchPrintfW (strsafe.h) rather than MSVC-only _snwprintf_s or the
    // MinGW-gated swprintf — this file compiles under both MSVC and MinGW/UCRT.
    static const wchar_t* const kCandidates[] = {
        L"%lsInstall-VipleSCHid.ps1",                 // deploy 佈局：exe 根層
        L"%lssc_hid_driver\\Install-VipleSCHid.ps1",  // build tree 佈局：子目錄
    };
    wchar_t scriptPath[MAX_PATH] = {};
    bool found = false;
    for (const wchar_t* fmt : kCandidates) {
        StringCchPrintfW(scriptPath, MAX_PATH, fmt, exePath);
        if (GetFileAttributesW(scriptPath) != INVALID_FILE_ATTRIBUTES) {
            found = true;
            break;
        }
    }
    if (!found) {
        snprintf(status, sizeof(status),
                 "installer: not found (looked in %ls and %lssc_hid_driver\\)", exePath, exePath);
        setInstallDiag(status);
        g_installReported.store(true, std::memory_order_relaxed);  // 同步結果，呼叫端已 append
        g_installDone.store(true, std::memory_order_release);
        return status;
    }

    try {
        std::thread(installDriverWorker, std::wstring(scriptPath)).detach();
    }
    catch (const std::exception& ex) {
        snprintf(status, sizeof(status), "installer=%ls thread launch failed: %s", scriptPath, ex.what());
        setInstallDiag(status);
        g_installReported.store(true, std::memory_order_relaxed);  // 同步結果，呼叫端已 append
        g_installDone.store(true, std::memory_order_release);
        return status;
    }
    snprintf(status, sizeof(status), "installer launched async: %ls (wait<=60s)", scriptPath);
    return status;
}

// Throttle timestamp for VipleSCHidOpen (file-scope so VipleSCHidResetOpenThrottle can reset it).
static ULONGLONG g_openLastAttempt = 0;

void VipleSCHidResetOpenThrottle(void) {
    g_openLastAttempt = 0;
}

// Opens a direct handle bypassing the per-session throttle.
// Use only for the dedicated polling thread that manages its own handle lifecycle.
VIPLE_SCHID_HANDLE VipleSCHidOpenDirect(void) {
    HANDLE h = openHidByVidPid(SC_VID, SC_PID);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    return new VipleSCHidCtx{h, nullptr, nullptr};
}

// Force Steam to re-enumerate VipleSCHid by disabling and re-enabling its
// root-enumerated PDO. This makes Steam re-run GetControllerInfo handshake
// with a newly-connected proxy already in place.
// Returns 1 on success, 0 if the device could not be found or cycled.
int VipleSCHidForceReconnect(void) {
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO devInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr,
                                            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return 0;

    DEVINST targetInst = 0;
    bool found = false;

    for (DWORD i = 0; !found; ++i) {
        SP_DEVICE_INTERFACE_DATA ifaceData = {};
        ifaceData.cbSize = sizeof(ifaceData);
        if (!SetupDiEnumDeviceInterfaces(devInfo, nullptr, &hidGuid, i, &ifaceData)) break;

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, nullptr, 0, &requiredSize, nullptr);
        if (!requiredSize) continue;

        auto* detail = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(
            HeapAlloc(GetProcessHeap(), 0, requiredSize));
        if (!detail) continue;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        SP_DEVINFO_DATA devInfoData = {};
        devInfoData.cbSize = sizeof(devInfoData);
        if (SetupDiGetDeviceInterfaceDetailW(devInfo, &ifaceData, detail, requiredSize, nullptr, &devInfoData)) {
            HANDLE h0 = CreateFileW(detail->DevicePath, 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h0 != INVALID_HANDLE_VALUE) {
                HIDD_ATTRIBUTES attrs = {}; attrs.Size = sizeof(attrs);
                if (HidD_GetAttributes(h0, &attrs) &&
                    attrs.VendorID == SC_VID && attrs.ProductID == SC_PID) {
                    targetInst = devInfoData.DevInst;
                    found = true;
                }
                CloseHandle(h0);
            }
        }
        HeapFree(GetProcessHeap(), 0, detail);
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    if (!found) return 0;

    // Navigate up to the root-enumerated PDO (parent of the HIDClass PDO).
    DEVINST parentInst = 0;
    if (CM_Get_Parent(&parentInst, targetInst, 0) != CR_SUCCESS)
        parentInst = targetInst;  // fallback: cycle the HIDClass PDO itself

    // Cycle the device: Steam receives DeviceRemoved + DeviceAdded notifications
    // and re-runs GetControllerInfo with our proxy now active.
    CM_Disable_DevNode(parentInst, 0);
    Sleep(150);
    CM_Enable_DevNode(parentInst, 0);
    // §SC-HID Round 1：enable 之後 driver 的裝置 context 是新的（0x05 統計計數
    // 器歸零）。統計改為每次 GetFeature(0x05) 直接讀絕對值，這裡不需要重設任何
    // user-mode 基準。
    Sleep(300);

    // Allow VipleSCHidOpen to proceed immediately after re-enumeration.
    VipleSCHidResetOpenThrottle();
    return 1;
}

VIPLE_SCHID_HANDLE VipleSCHidOpen(void) {
    // The caller lazy-opens on EVERY incoming report while the device stays
    // unopenable — without a throttle that is a full enumeration + probe storm
    // at input-report rate (and the old code even re-ran the installer +
    // Sleep(2000) per report, which is where the "retry every 2s" cadence in
    // the logs came from). Real attempts at most every 5 s; throttled calls
    // leave the diag empty so the caller knows not to log them.
    ULONGLONG now = GetTickCount64();
    if (g_openLastAttempt != 0 && now - g_openLastAttempt < 5000) {
        g_diag[0] = '\0';
        return nullptr;
    }
    g_openLastAttempt = now;

    HANDLE hDev = openHidByVidPid(SC_VID, SC_PID);
    if (hDev == INVALID_HANDLE_VALUE) {
        // Driver not installed — kick off the auto-install ONCE per process, on
        // its own detached thread. No Sleep / wait here: this runs on the input
        // task_pool thread and must not stall the pipeline (§SC-HID WP2.4 審查
        // 修正 major). The next (throttled, >=5 s later) VipleSCHidOpen retries
        // the probe naturally and picks up the result below.
        if (!g_installLaunched.exchange(true)) {
            diagAppend(" | %s", tryInstallDriverAsync());
        }
    }
    // §SC-HID WP2.4：安裝結果只回報一次——append 進第一個「安裝已完成之後」的
    // open 嘗試的 diag（成功 → "device opened: …" 行；失敗 → "not found: …" 行）。
    if (g_installDone.load(std::memory_order_acquire) &&
        !g_installReported.exchange(true)) {
        std::lock_guard<std::mutex> lk(installMutex());
        if (g_installDiag[0] != '\0') diagAppend(" | %s", g_installDiag);
    }
    if (hDev == INVALID_HANDLE_VALUE) return nullptr;

    auto* ctx = new VipleSCHidCtx{hDev, nullptr, nullptr};
    return ctx;
}

// §SC-HID WP2：寫入/輪詢統計。舊碼假設「單一 input 執行緒、不用鎖」，但實際上
// VipleSCHidWrite 同時被 input 執行緒（真實 report）與 poll 執行緒（keepalive
// 重放）呼叫、VipleSCHidWriteDiag 又會在 session 解構子上跑——plain 計數是
// data race、共用的 diag 字串緩衝更會被撕裂。改 std::atomic + thread-local 緩衝。
static std::atomic<unsigned long> g_writeOk{0}, g_writeSkipped{0}, g_writeFailed{0}, g_lastWriteErr{0};
static std::atomic<uint8_t> g_lastSkippedId{0};
static std::atomic<unsigned long> g_notifyPolls{0}, g_notifyPollErrs{0}, g_notifyEvents{0};
static std::atomic<unsigned long> g_responsesDelivered{0};

// §SC-HID Round 1：driver 端統計不再從 0x03 尾段解迴繞累計（那條路徑已刪），
// 改由 VipleSCHidReadDriverStats() 每次 GetFeature(0x05) 讀絕對值；user-mode
// 這邊不保留任何 driver 計數狀態。

void VipleSCHidGetStats(VipleSCHidStats* out) {
    if (!out) return;
    out->writeOk            = g_writeOk.load(std::memory_order_relaxed);
    out->writeSkipped       = g_writeSkipped.load(std::memory_order_relaxed);
    out->writeFailed        = g_writeFailed.load(std::memory_order_relaxed);
    out->lastWriteErr       = g_lastWriteErr.load(std::memory_order_relaxed);
    out->lastSkippedId      = g_lastSkippedId.load(std::memory_order_relaxed);
    out->notifyPolls        = g_notifyPolls.load(std::memory_order_relaxed);
    out->notifyPollErrs     = g_notifyPollErrs.load(std::memory_order_relaxed);
    out->notifyEvents       = g_notifyEvents.load(std::memory_order_relaxed);
    out->responsesDelivered = g_responsesDelivered.load(std::memory_order_relaxed);
}

const char* VipleSCHidWriteDiag(void) {
    // vsnprintf-free: tiny fixed format, built on demand. thread_local：input
    // 執行緒、poll 執行緒與 session 解構子可能同時呼叫。
    static thread_local char diag[160];
    snprintf(diag, sizeof(diag),
             "ok=%lu skipped=%lu(lastId=0x%02X) failed=%lu lastErr=%lu",
             g_writeOk.load(std::memory_order_relaxed),
             g_writeSkipped.load(std::memory_order_relaxed),
             (unsigned)g_lastSkippedId.load(std::memory_order_relaxed),
             g_writeFailed.load(std::memory_order_relaxed),
             g_lastWriteErr.load(std::memory_order_relaxed));
    return diag;
}

const char* VipleSCHidFeatureDiag(void) {
    static thread_local char diag[128];
    snprintf(diag, sizeof(diag),
             "featureEvts=%lu responses=%lu pollErrs=%lu",
             g_notifyEvents.load(std::memory_order_relaxed),
             g_responsesDelivered.load(std::memory_order_relaxed),
             g_notifyPollErrs.load(std::memory_order_relaxed));
    return diag;
}

int VipleSCHidWrite(VIPLE_SCHID_HANDLE h, const uint8_t* data, int len) {
    if (!h || len <= 0) return 0;
    auto* ctx = static_cast<VipleSCHidCtx*>(h);

    // The virtual device's descriptor declares OUTPUT report ids 0x45 and
    // (since driver 1.0.5.0, §SC-HID WP0.3) 0x42 — the gamepad state as emitted
    // through the Puck wireless receiver; SDL treats 0x42 and 0x45 as the same
    // TritonMTUNoQuat_t layout. The client forwards EVERY vendor interface of
    // the physical SC + Puck, so reports with other ids (battery 0x43, wireless
    // status 0x46/0x79, telemetry 0x7B, …) do come through — HIDClass would
    // reject those writes with ERROR_INVALID_PARAMETER every time, and treating
    // that as "device broke" caused a close/re-open loop that dropped all input.
    // Skip them and return 2 (distinct from a real injection, so the caller
    // does not log "first report injected" / refresh the keepalive cache on
    // traffic that never reached the device — that masked the 0x42 case, F17).
    const uint8_t id = data[0];
    if (id != 0x45 && id != 0x42) {
        g_writeSkipped.fetch_add(1, std::memory_order_relaxed);
        g_lastSkippedId.store(id, std::memory_order_relaxed);
        return 2;
    }

    // gen-2 SC: the forwarded wire payload already begins with the HID report id
    // (0x45 / 0x42) at byte 0 — it IS a report-id'd report, so we do NOT prepend
    // a 0 (that was the old 0x1102 report-id-0 convention). Write it as a 64-byte
    // OUTPUT report (OutputReportByteLength=64); the driver takes the leading
    // 54 bytes ([id]+53 data) to complete the pending INPUT read.
    uint8_t buf[64] = {};
    int n = (len > 64) ? 64 : len;
    memcpy(buf, data, n);

    DWORD written = 0;
    if (WriteFile(ctx->hDev, buf, sizeof(buf), &written, nullptr)) {
        g_writeOk.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }

    DWORD e = GetLastError();
    g_writeFailed.fetch_add(1, std::memory_order_relaxed);
    g_lastWriteErr.store(e, std::memory_order_relaxed);
    // Only treat "the device object is gone" class errors as fatal; parameter
    // rejections and transient errors keep the (still valid) handle.
    switch (e) {
        case ERROR_DEVICE_NOT_CONNECTED:  // 1167
        case ERROR_FILE_NOT_FOUND:        // 2
        case ERROR_INVALID_HANDLE:        // 6
        case ERROR_OPERATION_ABORTED:     // 995 (device removal cancels I/O)
        case ERROR_NO_SUCH_DEVICE:        // 433 (§SC-HID WP2：PDO 已拆、handle 成孤兒)
        case ERROR_DEV_NOT_EXIST:         // 55  (§SC-HID WP2：disable/enable 週期中)
            return -1;
        default:
            return 0;
    }
}

// §SC-HID Phase 2C：輪詢 driver 的待辦 Steam feature 事件（report 0x03）。
// 注意：用 input 流量 piggyback 呼叫，頻率約 input rate；driver 端有 ring 緩衝。
// §SC-HID Round 1：0x03 是純事件（driver 統計改走 0x05）；GetFeature 失敗回 -1
// 讓 poll 執行緒能數「連續失敗」並主動 close/reopen（舊碼失敗與無事件同回 0，
// handle 死掉後 poll 執行緒永遠不會察覺）。
int VipleSCHidPollNotify(VIPLE_SCHID_HANDLE h, uint8_t* op, uint8_t* reportId,
                         uint8_t* seq, uint8_t* query, int* qlen) {
    if (!h) return -1;
    auto* ctx = static_cast<VipleSCHidCtx*>(h);
    uint8_t buf[64] = {};
    buf[0] = 0x03;  // requested report id = notify channel
    if (!HidD_GetFeature(ctx->hDev, buf, sizeof(buf))) {
        g_notifyPollErrs.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    g_notifyPolls.fetch_add(1, std::memory_order_relaxed);
    // layout: [0x03][op][reportId][seq][queryLen][query≤59]; op==0 → 無事件
    if (buf[1] == 0) return 0;
    g_notifyEvents.fetch_add(1, std::memory_order_relaxed);
    if (op)       *op       = buf[1];
    if (reportId) *reportId = buf[2];
    if (seq)      *seq      = buf[3];
    int n = buf[4];
    if (n > 59) n = 59;  // query 區 = bytes 5..63
    if (query && qlen) {
        memcpy(query, buf + 5, n);
        *qlen = n;
    } else if (qlen) {
        *qlen = n;
    }
    return 1;
}

// §SC-HID Round 1：讀 driver 端統計（Feature 0x05，佈局見 VipleSCHid.h）。
// 只由 poll 執行緒每 10 s 呼叫一次；LE16 直接用（driver 計數器隨裝置 context
// 建立歸零，ForceReconnect 後自然重新起算，不需 user-mode 解迴繞）。
int VipleSCHidReadDriverStats(VIPLE_SCHID_HANDLE h, VipleSCHidDrvStats* out, unsigned long* err) {
    if (err) *err = 0;
    if (out) memset(out, 0, sizeof(*out));
    if (!h || !out) {
        if (err) *err = ERROR_INVALID_PARAMETER;
        return 0;
    }
    auto* ctx = static_cast<VipleSCHidCtx*>(h);
    uint8_t buf[64] = {};
    buf[0] = 0x05;  // requested report id = driver stats
    if (!HidD_GetFeature(ctx->hDev, buf, sizeof(buf))) {
        // 舊 driver（1.0.4.0）沒宣告 0x05 → HIDClass 直接拒絕（typ. 87 / 1784）。
        if (err) *err = GetLastError();
        return 0;
    }
    auto le16 = [](const uint8_t* p) -> uint16_t {
        return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    };
    out->setCount        = le16(buf + 1);
    out->getCount        = le16(buf + 3);
    out->lastReportId    = buf[5];
    out->ringDepth       = buf[6];
    out->responsePending = buf[7];
    out->responseReady   = buf[8];
    out->getEvtPushed    = le16(buf + 9);
    out->delivered       = le16(buf + 11);
    out->getGated        = le16(buf + 13);
    out->zeroDropped     = le16(buf + 15);
    // driver 寫 [17..18] = 0x05,0x01 = LE16 0x0105 → major 在高位元組 [18]、minor 在 [17]
    out->verMajor        = buf[18];
    out->verMinor        = buf[17];
    return 1;
}

// §SC-HID Phase 2C：把實體 SC 的 feature 回應交付給 driver（output report 0x04）。
// 走 WriteFile（proven），不走壞掉的 HidD_SetFeature。data 是實體 SC 的 64-byte
// feature 回應（data[0] 是它的 report id）；只送 data[1..63]，driver 重建 byte0=0x01。
int VipleSCHidDeliverResponse(VIPLE_SCHID_HANDLE h, uint8_t seq,
                              const uint8_t* data, int len) {
    if (!h || !data || len <= 0) return 0;
    (void)seq;  // Option B：driver 只存最新回應，seq 暫不參與比對（保留供未來 parking 用）
    auto* ctx = static_cast<VipleSCHidCtx*>(h);
    uint8_t buf[64] = {};
    buf[0] = 0x04;  // output report id = response-delivery channel
    int n = (len > 64) ? 63 : (len - 1);
    if (n > 63) n = 63;
    if (n > 0) memcpy(buf + 1, data + 1, n);
    DWORD written = 0;
    if (!WriteFile(ctx->hDev, buf, sizeof(buf), &written, nullptr)) return 0;
    g_responsesDelivered.fetch_add(1, std::memory_order_relaxed);
    return 1;
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
