/*
 * §SC-HID: VipleSCHid_Driver.cpp — UMDF2 HID minidriver
 *
 * 呈現虛擬 USB HID 裝置：
 *   VendorID  = 0x28DE  (Valve)
 *   ProductID = 0x1102  (Steam Controller wired)
 *
 * 由 in-box Microsoft-signed mshidumdf.sys reflector 托管，
 * 不需要 kernel driver 簽章；只需 admin + 自簽憑證安裝。
 *
 * 資料流：
 *   Sunshine.exe 呼叫 WriteFile (→ IOCTL_HID_WRITE_REPORT)
 *     → 本 driver 取出等待中的 IOCTL_HID_READ_REPORT
 *     → 以 write 資料完成該 read request
 *     → Steam Input 的 ReadFile 取得 64-byte input report
 *
 * 建置：MSVC + WDK（WindowsUserModeDriver10.0 toolset）
 *   msbuild VipleSCHid_Driver.vcxproj /p:Configuration=Release /p:Platform=x64
 *   或直接跑 Build-ScHidDriver.ps1
 */

#define UMDF_USING_NTSTATUS
#include <ntstatus.h>
#include <windows.h>
#include <wdf.h>
#include <hidport.h>   // IOCTL_HID_* codes + HID_DESCRIPTOR / HID_DEVICE_ATTRIBUTES

// Steam Controller wired USB 識別碼
#define SC_VID  0x28DE
#define SC_PID  0x1102

// Steam Controller HID report descriptor（vendor-defined 64-byte）
// Interface 2 的 wired 格式。Steam Input 用 VID/PID allowlist 識別，
// 不靠 descriptor Usage items，所以用 vendor-defined page 即可。
static const UCHAR g_HidReportDescriptor[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor-Defined 0xFF00)
    0x09, 0x01,        // Usage (Vendor Usage 1)
    0xA1, 0x01,        // Collection (Application)
    // Input report：64 bytes
    0x09, 0x02,        //   Usage (Vendor Usage 2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x40,        //   Report Count (64)
    0x81, 0x02,        //   Input (Data, Var, Abs)
    // Feature report：64 bytes（lizard-mode 控制 + haptics 下行通道）
    0x09, 0x03,        //   Usage (Vendor Usage 3)
    0xB1, 0x02,        //   Feature (Data, Var, Abs)
    0xC0               // End Collection
};

static HID_DEVICE_ATTRIBUTES g_Attributes = {
    sizeof(HID_DEVICE_ATTRIBUTES),
    SC_VID,   // VendorID
    SC_PID,   // ProductID
    0x0100    // VersionNumber
};

// 裝置上下文：存放等待中 ReadReport 的 manual queue
typedef struct _DEVICE_CONTEXT {
    WDFQUEUE PendingReadQueue;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// ── WDF 事件宣告 ─────────────────────────────────────────────────────────────
EVT_WDF_DRIVER_DEVICE_ADD                   EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoInternalDeviceControl;

// ── DriverEntry ───────────────────────────────────────────────────────────────
// UMDF2 的進入點契約：簽章必須是 (PDRIVER_OBJECT, PUNICODE_STRING) 且為 extern "C"
// —— WdfDriverStubUm.lib 以未修飾 C 名稱、用 PDRIVER_OBJECT 呼叫它。若用 C++
// 連結（會 name-mangle）或用 (WDFDRIVER, PWDFDRIVER_CONFIG) 簽章，會 LNK2019。
// 並且必須實際呼叫 WdfDriverCreate 註冊 EvtDeviceAdd，否則框架永不初始化。
extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject,
                                _In_ PUNICODE_STRING RegistryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath,
                           WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

// ── EvtDeviceAdd ──────────────────────────────────────────────────────────────
NTSTATUS EvtDeviceAdd(IN WDFDRIVER Driver, IN PWDFDEVICE_INIT DeviceInit) {
    UNREFERENCED_PARAMETER(Driver);

    // 注意：UMDF2 沒有 WdfDeviceInitSetDeviceType（KMDF-only API，UMDF2 標頭
    // 不提供 → C3861）。HID minidriver 不需要設定 device type，省略即可。

    // 裝置上下文大小
    WDF_OBJECT_ATTRIBUTES devAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttrs, DEVICE_CONTEXT);

    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, &devAttrs, &device);
    if (!NT_SUCCESS(status)) return status;

    PDEVICE_CONTEXT ctx = GetDeviceContext(device);

    // ── Default queue：處理描述符 IOCTL（parallel dispatch）────────────────
    WDF_IO_QUEUE_CONFIG queueCfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueCfg, WdfIoQueueDispatchParallel);
    queueCfg.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;

    WDFQUEUE defaultQueue;
    status = WdfIoQueueCreate(device, &queueCfg, WDF_NO_OBJECT_ATTRIBUTES, &defaultQueue);
    if (!NT_SUCCESS(status)) return status;

    // ── Manual queue：存放等待中的 IOCTL_HID_READ_REPORT────────────────────
    WDF_IO_QUEUE_CONFIG_INIT(&queueCfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueCfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->PendingReadQueue);
    return status;
}

// ── EvtIoInternalDeviceControl ────────────────────────────────────────────────
VOID EvtIoInternalDeviceControl(
    IN WDFQUEUE   Queue,
    IN WDFREQUEST Request,
    IN size_t     OutputBufferLength,
    IN size_t     InputBufferLength,
    IN ULONG      IoControlCode)
{
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT ctx = GetDeviceContext(device);

    NTSTATUS  status = STATUS_INVALID_DEVICE_REQUEST;
    WDFMEMORY mem    = nullptr;
    size_t    transferred = 0;

    switch (IoControlCode) {

    // ── HID 裝置 / 描述符查詢 ─────────────────────────────────────────────
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR: {
        HID_DESCRIPTOR desc = {};
        desc.bLength          = sizeof(HID_DESCRIPTOR);
        desc.bDescriptorType  = HID_HID_DESCRIPTOR_TYPE;
        desc.bcdHID           = 0x0101;
        desc.bCountry         = 0;
        desc.bNumDescriptors  = 1;
        desc.DescriptorList[0].bReportType   = HID_REPORT_DESCRIPTOR_TYPE;
        desc.DescriptorList[0].wReportLength = (USHORT)sizeof(g_HidReportDescriptor);

        if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &mem))) {
            WdfMemoryCopyFromBuffer(mem, 0, &desc, sizeof(desc));
            transferred = sizeof(desc);
        }
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_HID_GET_REPORT_DESCRIPTOR: {
        if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &mem))) {
            WdfMemoryCopyFromBuffer(mem, 0,
                const_cast<UCHAR*>(g_HidReportDescriptor),
                sizeof(g_HidReportDescriptor));
            transferred = sizeof(g_HidReportDescriptor);
        }
        status = STATUS_SUCCESS;
        break;
    }

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
        if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &mem))) {
            WdfMemoryCopyFromBuffer(mem, 0, &g_Attributes, sizeof(g_Attributes));
            transferred = sizeof(g_Attributes);
        }
        status = STATUS_SUCCESS;
        break;
    }

    // ── Read report：Steam Input 的 ReadFile 觸發這個 IOCTL ────────────────
    // 把 request 放入 manual queue，等 WriteReport 進來時再完成
    case IOCTL_HID_READ_REPORT: {
        NTSTATUS fwdStatus = WdfRequestForwardToIoQueue(Request, ctx->PendingReadQueue);
        if (!NT_SUCCESS(fwdStatus)) {
            WdfRequestComplete(Request, fwdStatus);
        }
        return; // 不呼叫下方的 WdfRequestCompleteWithInformation
    }

    // ── Write report：Sunshine 的 WriteFile 觸發這個 IOCTL ────────────────
    // 取出等待中的 read request，以 write 資料完成它（loopback 注入）
    case IOCTL_HID_WRITE_REPORT: {
        WDFMEMORY writeMem = nullptr;
        if (!NT_SUCCESS(WdfRequestRetrieveInputMemory(Request, &writeMem))) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        size_t  writeSize = 0;
        PVOID   writeData = WdfMemoryGetBuffer(writeMem, &writeSize);

        // 嘗試取出等待中的 read request
        WDFREQUEST readRequest = nullptr;
        NTSTATUS   deqStatus   = WdfIoQueueRetrieveNextRequest(ctx->PendingReadQueue, &readRequest);

        if (NT_SUCCESS(deqStatus) && readRequest != nullptr) {
            WDFMEMORY readMem = nullptr;
            if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(readRequest, &readMem))) {
                // HID write 前面有 1-byte report ID（0），跳過它
                PUCHAR src    = (PUCHAR)writeData;
                SIZE_T srcOff = (writeSize > 0 && src[0] == 0) ? 1 : 0;
                SIZE_T toCopy = min((SIZE_T)64, writeSize - srcOff);
                if (toCopy > 0) {
                    WdfMemoryCopyFromBuffer(readMem, 0, src + srcOff, toCopy);
                }
            }
            WdfRequestCompleteWithInformation(readRequest, STATUS_SUCCESS, 64);
        }
        // write request 本身永遠成功
        status = STATUS_SUCCESS;
        break;
    }

    // ── Feature report（lizard-mode 控制 / haptics 下行）─────────────────
    // 目前先回傳 success；完整 haptics 下行在 SS_SC_HID_FEATURE 路徑實作
    case IOCTL_HID_GET_FEATURE:
    case IOCTL_HID_SET_FEATURE:
        status = STATUS_SUCCESS;
        break;

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestCompleteWithInformation(Request, status, transferred);
}
