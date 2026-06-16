/*
 * §SC-HID: VipleSCHid_Driver.cpp — UMDF2 HID minidriver
 *
 * 呈現虛擬 USB HID 裝置：
 *   VendorID  = 0x28DE  (Valve)
 *   ProductID = 0x1302  (gen-2 Steam Controller "Triton", wired)
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
// 其餘隨輸入變動。host Steam 靠 VID/PID(0x1302) 識別並自行解析 raw report 0x45。
//
// §SC-HID Phase 2C：透明雙向 feature proxy。為了讓 Steam 的 GetControllerInfo 握手
// 能拿到實體 SC 的真實回應，driver 多開兩條「控制」report（都維持 64-byte，避免動到
// FeatureReportByteLength/OutputReportByteLength 影響 Steam 的 0x01 / 既有 0x45 注入）：
//   0x45  Input(54)/Output(64) — 實體 SC gamepad 注入（Sunshine WriteFile）
//   0x01  Feature(64)          — Steam 握手讀；driver 回 ctx->LastResponse
//   0x03  Feature(64)          — Sunshine GetFeature(0x03) 取 Steam 待辦 feature 事件（notify）
//   0x04  Output(64)           — Sunshine WriteFile(0x04) 交付實體 SC 的 feature 回應
// （舊 0x02「seed」假通道已移除：它靠 HidD_SetFeature，實機回 result=0 失敗，改走 0x04 WriteFile。）
static const UCHAR g_HidReportDescriptor[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor-Defined 0xFF00)
    0x09, 0x01,        // Usage (0x01)
    0xA1, 0x01,        // Collection (Application)
    // --- Input/Output: vendor gamepad report id 0x45 ---
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
    // --- Feature: Triton control report id 0x01 (Steam's GetControllerInfo polls this) ---
    0x85, 0x01,        //   Report ID (0x01) — Triton feature/control report
    0x09, 0x01,        //   Usage (0x01)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x3F,        //   Report Count (63)  → Feature = id + 63 = 64 bytes
    0xB1, 0x02,        //   Feature (Data, Var, Abs)
    // --- Feature notify: report id 0x03 (driver → Sunshine; carries pending Steam feature op) ---
    0x85, 0x03,        //   Report ID (0x03)
    0x09, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x3F,        //   Report Count (63)  → Feature = id + 63 = 64 bytes
    0xB1, 0x02,        //   Feature (Data, Var, Abs)
    // --- Output delivery: report id 0x04 (Sunshine → driver; real SC feature response) ---
    0x85, 0x04,        //   Report ID (0x04)
    0x09, 0x01,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x3F,        //   Report Count (63)  → Output = id + 63 = 64 bytes
    0x91, 0x02,        //   Output (Data, Var, Abs)
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

// §SC-HID Phase 2C：driver → Sunshine 的待辦事件 ring buffer。
// Steam 對 report 0x01 做 SET_FEATURE / GET_FEATURE 時，driver push 一筆事件；
// Sunshine 以 GetFeature(report 0x03) 在 input 流量上 piggyback 輪詢 pop。
// 每筆事件塞進一個 64-byte notify report：[0x03][op][reportId][seq][queryLen][query…]
#define SC_EVT_OP_NONE 0
#define SC_EVT_OP_GET  1   // Steam GET_FEATURE(0x01) — 請 client 重讀實體 SC 刷新回應
#define SC_EVT_OP_SET  2   // Steam SET_FEATURE(0x01, query) — 請 client 轉送 query 給實體 SC
#define SC_EVT_QUERY_MAX 59   // 64 - 5(id,op,reportId,seq,queryLen)
#define SC_EVT_RING 16

typedef struct _SC_EVT {
    UCHAR op;
    UCHAR reportId;
    UCHAR seq;
    UCHAR queryLen;
    UCHAR query[SC_EVT_QUERY_MAX];
} SC_EVT;

// 裝置上下文：等待中 ReadReport 的 manual queue + feature proxy 狀態
typedef struct _DEVICE_CONTEXT {
    WDFQUEUE PendingReadQueue;
    // Steam GET_FEATURE(report 0x01) 回傳的資料 = 實體 SC 經 Sunshine WriteFile(0x04) 交付的回應。
    // byte[0] 固定 0x01。未交付前回 zeros（Steam 會重試直到拿到真實資料）。
    UCHAR LastResponse[64];
    // 待辦事件 ring（Steam 動作 → Sunshine 輪詢取走）
    SC_EVT  EvtRing[SC_EVT_RING];
    ULONG   EvtHead;   // pop 位置
    ULONG   EvtTail;   // push 位置
    UCHAR   SeqCtr;
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

    // ── Default queue：處理描述符 / report IOCTL（sequential dispatch）──────
    // §SC-QUEUE-SERIAL-FIX (review batch 2)：Parallel dispatch 下 Steam 的
    // GET/SET_FEATURE(0x01)、Sunshine 的 GET_FEATURE(0x03) 輪詢與
    // WriteFile(0x45/0x04) 會多執行緒並發進 EvtIoDeviceControl，而
    // EvtRing/EvtHead/EvtTail/SeqCtr/LastResponse 全部無鎖——ScPushEvent
    // 與 0x03 pop 並發會撕裂 ring（重複 pop、跳號、半寫入事件）。本
    // driver 所有 handler 都是短同步操作；唯一的長命 request
    // （IOCTL_HID_READ_REPORT）立刻 forward 進 manual queue，forward 即
    // 釋放 sequential 配額，不會卡住 queue。Sequential 一次消除全部
    // 競態，比逐段加 WDFSPINLOCK 更不易漏。
    WDF_IO_QUEUE_CONFIG queueCfg;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueCfg, WdfIoQueueDispatchSequential);
    // mshidumdf.sys 將 HID Internal IOCTL 轉成普通 DeviceControl 再上送 UMDF layer；
    // UMDF2 的 queue 必須掛 EvtIoDeviceControl（不是 Internal），否則 IOCTL 永不分派。
    queueCfg.EvtIoDeviceControl = EvtIoDeviceControl;

    WDFQUEUE defaultQueue;
    status = WdfIoQueueCreate(device, &queueCfg, WDF_NO_OBJECT_ATTRIBUTES, &defaultQueue);
    if (!NT_SUCCESS(status)) return status;

    // ── Manual queue：存放等待中的 IOCTL_HID_READ_REPORT────────────────────
    WDF_IO_QUEUE_CONFIG_INIT(&queueCfg, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(device, &queueCfg, WDF_NO_OBJECT_ATTRIBUTES, &ctx->PendingReadQueue);
    if (!NT_SUCCESS(status)) return status;

    // 初始化 feature proxy 狀態（§SC-HID Phase 2C）
    RtlZeroMemory(ctx->LastResponse, sizeof(ctx->LastResponse));
    ctx->LastResponse[0] = 0x01;   // report id Steam reads
    RtlZeroMemory(ctx->EvtRing, sizeof(ctx->EvtRing));
    ctx->EvtHead = 0;
    ctx->EvtTail = 0;
    ctx->SeqCtr  = 0;
    return STATUS_SUCCESS;
}

// Push 一筆待辦事件給 Sunshine（Steam SET/GET on report 0x01 時呼叫）。
// ring 滿就覆蓋最舊（drop oldest）——Sunshine 以 input 流量輪詢，正常不會滿。
static void ScPushEvent(PDEVICE_CONTEXT ctx, UCHAR op, UCHAR reportId,
                        const UCHAR* query, ULONG queryLen) {
    SC_EVT* e = &ctx->EvtRing[ctx->EvtTail % SC_EVT_RING];
    e->op       = op;
    e->reportId = reportId;
    e->seq      = ++ctx->SeqCtr;
    ULONG n = queryLen > SC_EVT_QUERY_MAX ? SC_EVT_QUERY_MAX : queryLen;
    e->queryLen = (UCHAR)n;
    RtlZeroMemory(e->query, sizeof(e->query));
    if (query && n) RtlCopyMemory(e->query, query, n);
    ctx->EvtTail++;
    // 若 tail 追過 head（ring 滿），head 跟進丟掉最舊一筆
    if (ctx->EvtTail - ctx->EvtHead > SC_EVT_RING) ctx->EvtHead = ctx->EvtTail - SC_EVT_RING;
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

        // §SC-HID Phase 2C：report id 0x04 = Sunshine 交付實體 SC 的 feature 回應。
        // 不完成任何 read，只把資料存進 LastResponse 供 Steam GET_FEATURE(0x01) 取用。
        // wire 上 byte[0]=0x04，其後 63 bytes 為實體 SC 回應的 byte[1..63]（byte0 是
        // report id，由本端重建為 0x01）。
        if (writeSize >= 1 && ((UCHAR*)writeData)[0] == 0x04) {
            ctx->LastResponse[0] = 0x01;
            size_t copyN = writeSize > 64 ? 63 : (writeSize - 1);
            if (copyN > 63) copyN = 63;
            if (copyN > 0) RtlCopyMemory(ctx->LastResponse + 1, (UCHAR*)writeData + 1, copyN);
            status = STATUS_SUCCESS;
            break;
        }

        // 嘗試取出等待中的 read request（report id 0x45 input 注入）
        WDFREQUEST readRequest = nullptr;
        NTSTATUS   deqStatus   = WdfIoQueueRetrieveNextRequest(ctx->PendingReadQueue, &readRequest);

        if (NT_SUCCESS(deqStatus) && readRequest != nullptr) {
            // gen-2 SC：output 與 input 同用 report id 0x45（69），byte0 即 report id。
            // 把 Sunshine 寫進來的 64-byte output report 前緣（含 report id 0x45）原樣
            // 餵給等待中的 input read；input report 長度 54 (= id + 53)，host Steam 讀到
            // [0x45][53 data]，與實體 gen-2 SC 一致。copy 量取兩 buffer 較小者。
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

    // ── Feature report：透明雙向 proxy（§SC-HID Phase 2C）────────────────
    // Steam 的 CGetControllerInfoWorkItem 流程：SET_FEATURE(0x01, query) 再
    // 迴圈 GET_FEATURE(0x01) 取回應。我們不在本地造假，而是把 Steam 的動作 push
    // 成事件、由 Sunshine（輪詢 report 0x03）轉送到實體 SC，回應再經 WriteFile(0x04)
    // 寫回 ctx->LastResponse；Steam 的 GET_FEATURE(0x01) 取走它（Steam 會重試直到非空）。
    //   report 0x01 = Steam 握手；report 0x03 = Sunshine 取待辦事件（notify）。
    //
    // 根本原因（§SC-HID root-cause）：UMDF2 minidriver 收到的 feature IOCTL code 是
    // IOCTL_UMDF_HID_SET/GET_FEATURE（HID_CTL_CODE(20/21)），而非 KM 用的
    // IOCTL_HID_SET/GET_FEATURE。兩者數值不同，故只列 KM code 的 switch 永遠落入
    // default → STATUS_NOT_SUPPORTED（host 診斷 err=50）。
    // 另一個關鍵：UMDF feature IOCTL 傳遞的 buffer 是 HID_XFER_PACKET 結構體
    //（{ PUCHAR reportBuffer; ULONG reportBufferLen; UCHAR reportId; }），
    // 而非原始 report bytes——reportId 在結構末尾，reportBuffer 是指向實際資料的指標。
    // vhidmini2 範例確認：兩種 code 共用同一處理函式，皆用 HID_XFER_PACKET 存取。
    case IOCTL_HID_SET_FEATURE:        // KM path（防禦性，UMDF 中不會觸發）
    case IOCTL_UMDF_HID_SET_FEATURE: { // UMDF path（mshidumdf.sys 實際送來的 code）
        PVOID inBuf  = nullptr;
        size_t inLen = 0;
        if (NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(HID_XFER_PACKET), &inBuf, &inLen))) {
            PHID_XFER_PACKET pkt = (PHID_XFER_PACKET)inBuf;
            UCHAR reqId = pkt->reportId;
            if (reqId == 0x01 && pkt->reportBuffer != nullptr && pkt->reportBufferLen > 0) {
                // Steam 送 query 給控制器 → 轉成 SET 事件交給 Sunshine 轉發到實體 SC。
                ULONG qn = pkt->reportBufferLen > SC_EVT_QUERY_MAX
                           ? SC_EVT_QUERY_MAX : pkt->reportBufferLen;
                ScPushEvent(ctx, SC_EVT_OP_SET, 0x01, (const UCHAR*)pkt->reportBuffer, qn);
            }
            // 其餘 report id 忽略。
        }
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_HID_GET_FEATURE:        // KM path（防禦性，UMDF 中不會觸發）
    case IOCTL_UMDF_HID_GET_FEATURE: { // UMDF path（mshidumdf.sys 實際送來的 code）
        PVOID outBuf  = nullptr;
        size_t outLen = 0;
        if (NT_SUCCESS(WdfRequestRetrieveOutputBuffer(Request, sizeof(HID_XFER_PACKET), &outBuf, &outLen))) {
            PHID_XFER_PACKET pkt = (PHID_XFER_PACKET)outBuf;
            UCHAR  reqId       = pkt->reportId;
            PVOID  reportBuf   = pkt->reportBuffer;
            ULONG  reportBufLen = pkt->reportBufferLen;
            if (reportBuf != nullptr && reportBufLen > 0) {
                size_t n = reportBufLen < 64 ? reportBufLen : 64;
                if (reqId == 0x03) {
                    // Sunshine 輪詢待辦事件。有事件就 pop 一筆填回；沒有則 op=NONE。
                    RtlZeroMemory(reportBuf, n);
                    ((UCHAR*)reportBuf)[0] = 0x03;
                    if (ctx->EvtHead != ctx->EvtTail && n >= 5) {
                        SC_EVT* e = &ctx->EvtRing[ctx->EvtHead % SC_EVT_RING];
                        ctx->EvtHead++;
                        UCHAR* o = (UCHAR*)reportBuf;
                        o[1] = e->op;
                        o[2] = e->reportId;
                        o[3] = e->seq;
                        o[4] = e->queryLen;
                        size_t qcopy = e->queryLen;
                        if (qcopy > n - 5) qcopy = n - 5;
                        if (qcopy > 0) RtlCopyMemory(o + 5, e->query, qcopy);
                    } else {
                        ((UCHAR*)reportBuf)[1] = SC_EVT_OP_NONE;
                    }
                } else {
                    // Steam 讀 report 0x01：回傳實體 SC 的最新回應，並 push 一筆 GET 事件
                    // 讓 Sunshine 請 client 重讀 SC 刷新（Steam 重試時就會拿到最新值）。
                    if (reqId == 0x01) ScPushEvent(ctx, SC_EVT_OP_GET, 0x01, nullptr, 0);
                    size_t copyN = n < sizeof(ctx->LastResponse) ? n : sizeof(ctx->LastResponse);
                    RtlCopyMemory(reportBuf, ctx->LastResponse, copyN);
                }
                transferred = reportBufLen;  // 告知 HidClass 實際寫入的位元組數
            }
        }
        status = STATUS_SUCCESS;
        break;
    }

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestSetInformation(Request, transferred);
    WdfRequestComplete(Request, status);
}
