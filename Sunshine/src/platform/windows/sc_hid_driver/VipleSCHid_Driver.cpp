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

// gen-2 (2025/2026) Steam Controller 識別碼。0x1302 = 控制器本體（USB 直連），
// 0x1303 = Bluetooth LE，0x1304 = Puck（無線接收器）。我們對 host 一律呈現 0x1302，
// 讓 host Steam 把它當「直連的新 SC」並套用 gen-2 Steam Input profile。
// （舊 2015 SC 是 0x1102，本驅動不再對準它。）
#define SC_VID  0x28DE
#define SC_PID  0x1302

// gen-2 Steam Controller 的 vendor gamepad input report 用 **Report ID 0x45 (69)**，
// 介面 UsagePage 0xFF00 / Usage 0x01。實測（在 .195 直接讀實體 0x1302）：不論本機 Steam
// 開或關，device 都串流 report id 0x45、54 bytes（= 1 id + 53 data）；byte0 固定 0x45、
// 其餘隨輸入變動。HidP caps 另外宣告一個 report 0x42(66)（有對應 HID usage）但**不串流**，
// 真正吐 gamepad 資料的是 0x45（vendor-opaque raw）。host Steam 靠 VID/PID(0x1302) 識別並
// 自行解析 raw report 0x45，所以欄位內容用 vendor-opaque（53 byte）即可、不需逐欄定義。
// Output=64、Feature=64（feature 實際 report id 為 0x01，haptics 下行時再細修；input passthrough
// 不需要）。
static const UCHAR g_HidReportDescriptor[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor-Defined 0xFF00)
    0x09, 0x01,        // Usage (0x01)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x45,        //   Report ID (0x45 / 69) — gen-2 SC streamed gamepad report
    0x09, 0x01,        //   Usage (0x01)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x35,        //   Report Count (53)  → Input  = id + 53 = 54 bytes
    0x81, 0x02,        //   Input (Data, Var, Abs)
    0x09, 0x01,        //   Usage (0x01)
    0x95, 0x3F,        //   Report Count (63)  → Output = id + 63 = 64 bytes
    0x91, 0x02,        //   Output (Data, Var, Abs)
    0x09, 0x01,        //   Usage (0x01)
    0x95, 0x3F,        //   Report Count (63)  → Feature = id + 63 = 64 bytes
    0xB1, 0x02,        //   Feature (Data, Var, Abs)
    0xC0               // End Collection
};

// gen-2 SC streamed gamepad report id (confirmed by reading the real device)
#define SC_REPORT_ID  0x45

static HID_DEVICE_ATTRIBUTES g_Attributes = {
    sizeof(HID_DEVICE_ATTRIBUTES),
    SC_VID,   // VendorID
    SC_PID,   // ProductID  (0x1302 = gen-2 SC)
    0x0100    // VersionNumber
};

// 裝置上下文：存放等待中 ReadReport 的 manual queue
typedef struct _DEVICE_CONTEXT {
    WDFQUEUE PendingReadQueue;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// ── WDF 事件宣告 ─────────────────────────────────────────────────────────────
EVT_WDF_DRIVER_DEVICE_ADD        EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

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

    // mshidumdf.sys 做為 lower filter 把 IOCTL_HID_* Internal IOCTL 轉送給本 UMDF driver，
    // 本 driver 是 FDO 之上的 filter——必須呼叫 WdfFdoInitSetFilter，否則 WdfDeviceCreate
    // 會以為我們要獨佔 FDO stack 的 DO，WUDFHost 載入時 HID class driver 拒絕 START。
    WdfFdoInitSetFilter(DeviceInit);

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
    // mshidumdf.sys 將 HID Internal IOCTL 轉成普通 DeviceControl 再上送 UMDF layer；
    // UMDF2 的 queue 必須掛 EvtIoDeviceControl（不是 Internal），否則 IOCTL 永不分派。
    queueCfg.EvtIoDeviceControl = EvtIoDeviceControl;

    WDFQUEUE defaultQueue;
    status = WdfIoQueueCreate(device, &queueCfg, WDF_NO_OBJECT_ATTRIBUTES, &defaultQueue);
    if (!NT_SUCCESS(status)) return status;

    // ── Manual queue：存放等待中的 IOCTL_HID_READ_REPORT────────────────────
    WDF_IO_QUEUE_CONFIG_INIT(&queueCfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueCfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->PendingReadQueue);
    return status;
}

// ── EvtIoDeviceControl ────────────────────────────────────────────────────────
// mshidumdf.sys 把所有 IOCTL_HID_* Internal IOCTLs 轉為普通 DeviceControl 上送；
// UMDF2 driver 只看得到 DeviceControl，不會收到 InternalDeviceControl。
VOID EvtIoDeviceControl(
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
        return; // 不呼叫下方的 WdfRequestSetInformation / WdfRequestComplete
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
            // gen-2 SC：output 與 input 同用 report id 66，byte0 即 report id。
            // 不再跳過 leading 0（那是舊 0x1102 的 report-id-0 模型）。把 Sunshine 寫進來
            // 的 64-byte output report 前緣（含 report id 66）原樣餵給等待中的 input read；
            // input report 長度 54 (= id + 53)，host Steam 讀到 [66][53 data]，與實體 gen-2
            // SC 一致。copy 量取 read buffer 與 write buffer 的較小者。
            WDFMEMORY readMem = nullptr;
            size_t    readLen = 0;
            if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(readRequest, &readMem))) {
                (void)WdfMemoryGetBuffer(readMem, &readLen);
                SIZE_T toCopy = min((SIZE_T)readLen, (SIZE_T)writeSize);
                if (toCopy > 0) {
                    WdfMemoryCopyFromBuffer(readMem, 0, writeData, toCopy);
                }
            }
            WdfRequestSetInformation(readRequest, readLen);  // input report 實際長度（54）
            WdfRequestComplete(readRequest, STATUS_SUCCESS);
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

    WdfRequestSetInformation(Request, transferred);
    WdfRequestComplete(Request, status);
}
