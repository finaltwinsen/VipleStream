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
//   0x42  Input(54)/Output(64) — 同上，Puck 原生 id（Round 1 Plan B 透傳通道）
//   0x01  Feature(64)          — Steam 握手讀；driver 回 ctx->LastResponse
//   0x03  Feature(64)          — Sunshine GetFeature(0x03) 取 Steam 待辦 feature 事件（notify）
//   0x04  Output(64)           — Sunshine WriteFile(0x04) 交付實體 SC 的 feature 回應
//   0x05  Feature(64)          — Sunshine GetFeature(0x05) 讀 driver 統計（Round 1；不佔 0x03 的 query 空間）
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
    // --- Input/Output: vendor gamepad report id 0x42 (§SC-HID Round 1, 2026-09-02) ---
    // 實測 Puck（0x1304）slot 吐的是 0x42 = ID_TRITON_CONTROLLER_STATE（與 0x45 同
    // TritonMTUNoQuat_t 佈局）。client 預設把 0x42 改標成 0x45 再送（kNormalize42），
    // 這裡另宣告 0x42 做 Plan B 原樣透傳通道；長度與 0x45 相同，不影響
    // Input/OutputReportByteLength。
    0x85, 0x42,        //   Report ID (0x42 / 66) — ID_TRITON_CONTROLLER_STATE
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
    // --- Feature stats: report id 0x05 (driver → Sunshine; counters, §SC-HID Round 1) ---
    0x85, 0x05,        //   Report ID (0x05)
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

// SC-HID identity fix 2026-07-02：VersionNumber 對齊實體 gen-2 SC（USB 直連
// bcdDevice = 0x0307，實測 .195 的 HID\VID_28DE&PID_1302&REV_0307）。
// 注意：Steam 端「看得到 VID/PID」的關鍵不在這個結構（HidD_GetAttributes 一直
// 都回 28DE:1302），而在裝置的 hardware id / interface path——HIDClass 對
// root-enumerated 裝置是拿「父節點 HWID 去掉 enumerator、前綴 HID\」當子節點
// hardware id。所以 INF/Install script 的 HWID 必須是 Viple\VID_28DE&PID_1302
//（見 VipleSCHid.inf 頂部說明）。
static HID_DEVICE_ATTRIBUTES g_Attributes = {
    sizeof(HID_DEVICE_ATTRIBUTES),
    SC_VID,   // VendorID
    SC_PID,   // ProductID  (0x1302 = gen-2 SC)
    0x0307    // VersionNumber = 實體 REV_0307
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
    // §SC-HID Round 1（2026-09-02）：GET_FEATURE(0x01) 一次性閘控——模擬實體 Puck。
    // 實測：實體 SC 對 SET 後 13~21 ms 才有回應，回應被讀走前 GET 一律失敗
    // （ERROR_GEN_FAILURE）。Steam 的 CGetControllerInfoWorkItem 本來就靠輪詢處理
    // 這個語義；虛擬裝置若在回應抵達前就以「成功 + 舊資料」回覆，Steam 會把舊
    // 資料當答案 → Read failure。所以：SET 在途且回應未到 → 回 STATUS_UNSUCCESSFUL
    // （user-mode 看到 err 31），回應經 WriteFile(0x04) 抵達 → 下一次 GET 回傳。
    BOOLEAN   ResponsePending;   // Steam 已 SET、client 回應尚未交付
    BOOLEAN   ResponseReady;     // 回應已交付、等 Steam 的下一次 GET 取走
    ULONGLONG PendingTick;       // SET 時刻（GetTickCount64），逾時 200 ms 解除閘控
    // 統計（0x03 notify 回應 bytes 60-63 帶給 Sunshine，證明 Steam 的 IOCTL 有進來）
    ULONG   SetIoctlCount;       // SET_FEATURE(0x01) 次數
    ULONG   GetIoctlCount;       // GET_FEATURE(0x01) 次數
    ULONG   GetGatedCount;       // GET(0x01) 因閘控回失敗的次數
    ULONG   GetEvtPushed;        // 無 SET 在途時 push 的 GET 事件數（節流後）
    ULONG   ResponseDelivered;   // WriteFile(0x04) 有效交付次數（全零者不計）
    ULONG   ResponseZeroDropped; // WriteFile(0x04) 全零 payload 被忽略次數
    ULONGLONG LastGetEvtTick;    // 上一筆 GET 事件時刻（節流用）
    UCHAR   LastFeatureReqId;    // 最後一次 0x01 feature IOCTL 的 report id（只記 SET/GET 0x01）
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
    ctx->ResponsePending  = FALSE;
    ctx->ResponseReady    = FALSE;
    ctx->PendingTick      = 0;
    ctx->SetIoctlCount    = 0;
    ctx->GetIoctlCount    = 0;
    ctx->GetGatedCount    = 0;
    ctx->GetEvtPushed     = 0;
    ctx->ResponseDelivered = 0;
    ctx->ResponseZeroDropped = 0;
    ctx->LastGetEvtTick   = 0;
    ctx->LastFeatureReqId = 0;
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

// ── UMDF feature IOCTL 的 buffer 慣例（§SC-HID Round 1 根因 RC2b，2026-09-02）──
// KM HID minidriver 收到的 HID_XFER_PACKET 含內嵌指標，UMDF 無法跨 process 封送，
// 所以 mshidumdf.sys 把它拆成兩個 buffer 再以 IOCTL_UMDF_HID_* 上送（MS vhidmini2
// 範例 driver_umdf2_util.c 的 RequestGetHidXferPacket_ToReadFromDevice /
// _ToWriteToDevice 是唯一權威）：
//   GET_FEATURE（driver 寫、app 讀）：reportId = input buffer 第一個 byte；
//                                    report   = output buffer（含 byte0 = report id）
//   SET_FEATURE（app 寫、driver 讀）：report   = input buffer（含 byte0 = report id）；
//                                    reportId = OutputBufferLength（藏在長度欄位，因為
//                                    driver 對 output buffer 沒有讀取權）
// 舊碼把原始 64-byte buffer 硬轉成 HID_XFER_PACKET，得到 reportBufferLen=0 → 兩個
// handler 的主體從未執行：Steam 的 GET 拿回自己傳入的 buffer（transferred=0）、SET
// 事件永不入 ring → host Steam 的 GetControllerInfo 握手 100% 失敗（每秒 Read failure、
// 11 秒 zombie 重開）。host 現場 user-mode 探測（2026-09-02）證實。
static NTSTATUS ScGetFeaturePacket_Read(_In_ WDFREQUEST Request,
                                        _Out_ UCHAR* reportId,
                                        _Out_ PVOID* reportBuf,
                                        _Out_ size_t* reportLen) {
    *reportId = 0; *reportBuf = nullptr; *reportLen = 0;
    WDFMEMORY inMem = nullptr;
    NTSTATUS st = WdfRequestRetrieveInputMemory(Request, &inMem);
    if (!NT_SUCCESS(st)) return st;
    size_t inLen = 0;
    PVOID inBuf = WdfMemoryGetBuffer(inMem, &inLen);
    if (!inBuf || inLen < 1) return STATUS_INVALID_BUFFER_SIZE;
    *reportId = *(UCHAR*)inBuf;
    WDFMEMORY outMem = nullptr;
    st = WdfRequestRetrieveOutputMemory(Request, &outMem);
    if (!NT_SUCCESS(st)) return st;
    *reportBuf = WdfMemoryGetBuffer(outMem, reportLen);
    if (!*reportBuf || *reportLen < 1) return STATUS_INVALID_BUFFER_SIZE;
    return STATUS_SUCCESS;
}

static NTSTATUS ScGetFeaturePacket_Write(_In_ WDFREQUEST Request,
                                         _Out_ UCHAR* reportId,
                                         _Out_ PVOID* reportBuf,
                                         _Out_ size_t* reportLen) {
    *reportId = 0; *reportBuf = nullptr; *reportLen = 0;
    WDFMEMORY outMem = nullptr;
    NTSTATUS st = WdfRequestRetrieveOutputMemory(Request, &outMem);
    if (NT_SUCCESS(st)) {
        size_t outLen = 0;
        (void)WdfMemoryGetBuffer(outMem, &outLen);
        *reportId = (UCHAR)outLen;
    }
    WDFMEMORY inMem = nullptr;
    st = WdfRequestRetrieveInputMemory(Request, &inMem);
    if (!NT_SUCCESS(st)) return st;
    *reportBuf = WdfMemoryGetBuffer(inMem, reportLen);
    if (!*reportBuf || *reportLen < 1) return STATUS_INVALID_BUFFER_SIZE;
    // 保險：OutputBufferLength 拿不到 report id 時退回 report 的 byte0
    if (*reportId == 0) *reportId = *(UCHAR*)*reportBuf;
    return STATUS_SUCCESS;
}

// GET_FEATURE(0x01) 閘控逾時：實體 Puck 的回應 13~21 ms 內就到；wire 往返 +
// client 輪詢一般 <80 ms。逾時後退回舊行為（回 LastResponse），不會比修改前更差。
#define SC_RESPONSE_PENDING_TIMEOUT_MS 400ULL
// 無 SET 在途的 Steam GET 會 push GET 事件請 client 重讀；Steam 輪詢 GET 可達每 ms 一次，
// 事件不節流會灌爆 ring 並讓 client 在鎖內連續輪詢實體 SC。500 ms 一筆足夠刷新。
#define SC_GET_EVT_THROTTLE_MS 500ULL

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

    // ── HID string 查詢（SC-HID identity fix 2026-07-02）──────────────────
    // 實體 gen-2 SC 的 iProduct = "Steam Controller"；Steam/SDL 的列舉會讀
    // product string 來顯示與識別，虛擬裝置之前沒實作 → HidD_GetProductString
    // 失敗，是「Steam 看到裝置但不當它是 SC」的候選因素之一。
    // 註：IOCTL_HID_GET_STRING 的 string id 放在 Type3InputBuffer 的指標值
    // 本身（METHOD_NEITHER 慣例），UMDF 的 Copy 動作拿不到它——一律回
    // product string。manufacturer/serial 查詢也會拿到同字串，只是顯示
    // 層面的小瑕疵，不影響識別。
    case IOCTL_HID_GET_STRING:
    case IOCTL_HID_GET_INDEXED_STRING: {
        static const WCHAR kProduct[] = L"Steam Controller";
        if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &mem))) {
            size_t cap = 0;
            WdfMemoryGetBuffer(mem, &cap);
            size_t n = sizeof(kProduct) <= cap ? sizeof(kProduct) : cap;
            WdfMemoryCopyFromBuffer(mem, 0, const_cast<WCHAR*>(kProduct), n);
            transferred = n;
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
            size_t copyN = writeSize > 64 ? 63 : (writeSize - 1);
            if (copyN > 63) copyN = 63;
            // §SC-HID Round 1：全零 payload（client 拿不到回應時的佔位）不得覆蓋
            // LastResponse、也不得解除閘控——否則會把暖機寫入的真實屬性洗成零，
            // 且 Steam 會把零當答案。Valve 回應 byte1 = 訊息 type，永遠非 0。
            BOOLEAN allZero = TRUE;
            for (size_t k = 1; k <= copyN; k++) {
                if (((UCHAR*)writeData)[k] != 0) { allZero = FALSE; break; }
            }
            if (copyN == 0 || allZero) {
                ctx->ResponseZeroDropped++;
                status = STATUS_SUCCESS;
                break;
            }
            ctx->LastResponse[0] = 0x01;
            RtlCopyMemory(ctx->LastResponse + 1, (UCHAR*)writeData + 1, copyN);
            if (copyN < 63) RtlZeroMemory(ctx->LastResponse + 1 + copyN, 63 - copyN);
            ctx->ResponseDelivered++;
            // 回應已到，解除 GET(0x01) 閘控，讓 Steam 的下一次 GET 取走。
            ctx->ResponseReady = TRUE;
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

    // ── Feature report：透明雙向 proxy（§SC-HID Phase 2C；Round 1 重寫 buffer 解析）──
    // Steam 的 CGetControllerInfoWorkItem 流程：SET_FEATURE(0x01, query) 再輪詢
    // GET_FEATURE(0x01) 取回應（實體裝置在回應到達前 GET 會失敗）。我們不在本地造假：
    // Steam 的 SET 變成事件、由 Sunshine（輪詢 report 0x03）轉送到實體 SC，回應再經
    // WriteFile(0x04) 寫回 ctx->LastResponse；期間 Steam 的 GET 依閘控回失敗，回應到
    // 了才回傳（見 DEVICE_CONTEXT 的 ResponsePending/ResponseReady 說明）。
    //   report 0x01 = Steam 握手；report 0x03 = Sunshine 取待辦事件（notify）+ 統計。
    // buffer 慣例見上方 ScGetFeaturePacket_Read/_Write 的說明（RC2b 根因）。
    case IOCTL_HID_SET_FEATURE:        // KM path（防禦性，UMDF 中不會觸發）
    case IOCTL_UMDF_HID_SET_FEATURE: { // UMDF path（mshidumdf.sys 實際送來的 code）
        UCHAR  reqId = 0;
        PVOID  rep   = nullptr;
        size_t repLen = 0;
        NTSTATUS pst = ScGetFeaturePacket_Write(Request, &reqId, &rep, &repLen);
        if (NT_SUCCESS(pst)) {
            if (reqId == 0x01) {
                ctx->LastFeatureReqId = reqId;
                ctx->SetIoctlCount++;
                // 事件 query = 完整 report（byte0 = 0x01 report id，之後是 Valve
                // FeatureReportHeader{type,length}+payload）；client 端會用 reportId 覆寫
                // byte0 後原樣 send_feature_report 給實體 SC。ring 一筆最多 59 bytes。
                ULONG qn = (ULONG)(repLen > SC_EVT_QUERY_MAX ? SC_EVT_QUERY_MAX : repLen);
                ScPushEvent(ctx, SC_EVT_OP_SET, 0x01, (const UCHAR*)rep, qn);
                ctx->ResponsePending = TRUE;
                ctx->ResponseReady   = FALSE;
                ctx->PendingTick     = GetTickCount64();
            }
            // 其餘 report id：接受但忽略（descriptor 只宣告 0x01/0x03 feature）。
            status = STATUS_SUCCESS;
        } else {
            status = pst;
        }
        break;
    }
    case IOCTL_HID_GET_FEATURE:        // KM path（防禦性，UMDF 中不會觸發）
    case IOCTL_UMDF_HID_GET_FEATURE: { // UMDF path（mshidumdf.sys 實際送來的 code）
        UCHAR  reqId = 0;
        PVOID  rep   = nullptr;
        size_t repLen = 0;
        NTSTATUS pst = ScGetFeaturePacket_Read(Request, &reqId, &rep, &repLen);
        if (!NT_SUCCESS(pst)) { status = pst; break; }
        size_t n = repLen < 64 ? repLen : 64;
        UCHAR* o = (UCHAR*)rep;

        if (reqId == 0x05) {
            // Sunshine 讀統計（Round 1）。獨立 report 讓 0x03 的 query 空間完整（59 bytes）。
            // 佈局（LE16）：[0]=0x05 [1..2]=SET(0x01) 次數 [3..4]=GET(0x01) 次數
            //   [5]=最後 0x01 feature op 的 report id [6]=ring 深度 [7]=ResponsePending
            //   [8]=ResponseReady [9..10]=GET 事件 push 數 [11..12]=有效回應交付數
            //   [13..14]=GET 被閘控次數 [15..16]=全零回應被忽略數 [17..18]=driver 版本 0x0105
            RtlZeroMemory(o, n);
            if (n >= 19) {
                o[0] = 0x05;
                o[1] = (UCHAR)(ctx->SetIoctlCount & 0xFF);      o[2] = (UCHAR)((ctx->SetIoctlCount >> 8) & 0xFF);
                o[3] = (UCHAR)(ctx->GetIoctlCount & 0xFF);      o[4] = (UCHAR)((ctx->GetIoctlCount >> 8) & 0xFF);
                o[5] = ctx->LastFeatureReqId;
                o[6] = (UCHAR)((ctx->EvtTail - ctx->EvtHead) & 0xFF);
                o[7] = ctx->ResponsePending ? 1 : 0;
                o[8] = ctx->ResponseReady ? 1 : 0;
                o[9] = (UCHAR)(ctx->GetEvtPushed & 0xFF);       o[10] = (UCHAR)((ctx->GetEvtPushed >> 8) & 0xFF);
                o[11] = (UCHAR)(ctx->ResponseDelivered & 0xFF); o[12] = (UCHAR)((ctx->ResponseDelivered >> 8) & 0xFF);
                o[13] = (UCHAR)(ctx->GetGatedCount & 0xFF);     o[14] = (UCHAR)((ctx->GetGatedCount >> 8) & 0xFF);
                o[15] = (UCHAR)(ctx->ResponseZeroDropped & 0xFF); o[16] = (UCHAR)((ctx->ResponseZeroDropped >> 8) & 0xFF);
                o[17] = 0x05; o[18] = 0x01;   // driver 1.0.5 → 0x0105 (LE)
            }
            transferred = n;
            status = STATUS_SUCCESS;
        } else if (reqId == 0x03) {
            // Sunshine 輪詢待辦事件。有事件就 pop 一筆填回；沒有則 op=NONE。
            // （統計改走 0x05，這裡不再佔用 bytes 60-63，query 完整 59 bytes。）
            RtlZeroMemory(o, n);
            o[0] = 0x03;
            if (ctx->EvtHead != ctx->EvtTail && n >= 5) {
                SC_EVT* e = &ctx->EvtRing[ctx->EvtHead % SC_EVT_RING];
                ctx->EvtHead++;
                o[1] = e->op;
                o[2] = e->reportId;
                o[3] = e->seq;
                o[4] = e->queryLen;
                size_t qcopy = e->queryLen;
                if (qcopy > n - 5) qcopy = n - 5;
                if (qcopy > 0) RtlCopyMemory(o + 5, e->query, qcopy);
            } else if (n >= 2) {
                o[1] = SC_EVT_OP_NONE;
            }
            transferred = n;
            status = STATUS_SUCCESS;
        } else if (reqId == 0x01) {
            ctx->GetIoctlCount++;
            ctx->LastFeatureReqId = reqId;
            ULONGLONG now = GetTickCount64();
            if (ctx->ResponsePending) {
                if (ctx->ResponseReady) {
                    // 回應已交付：一次性取走（模擬實體）。
                    ctx->ResponsePending = FALSE;
                    ctx->ResponseReady   = FALSE;
                } else if ((now - ctx->PendingTick) < SC_RESPONSE_PENDING_TIMEOUT_MS) {
                    // 回應在途：像實體裝置一樣讓 GET 失敗，Steam 會繼續輪詢。
                    ctx->GetGatedCount++;
                    status = STATUS_UNSUCCESSFUL;   // user-mode: ERROR_GEN_FAILURE(31)
                    transferred = 0;
                    break;
                } else {
                    // 逾時（client 沒回、控制器睡眠…）：解除閘控，退回舊行為。
                    ctx->ResponsePending = FALSE;
                    ctx->ResponseReady   = FALSE;
                }
            } else {
                // 無 SET 在途的 GET：回最近一次回應，並（節流 500 ms）push 一筆 GET 事件
                // 讓 client 重讀實體 SC 刷新（Steam 重試時拿到最新值）。
                if ((now - ctx->LastGetEvtTick) >= SC_GET_EVT_THROTTLE_MS) {
                    ctx->LastGetEvtTick = now;
                    ctx->GetEvtPushed++;
                    ScPushEvent(ctx, SC_EVT_OP_GET, 0x01, nullptr, 0);
                }
            }
            size_t copyN = n < sizeof(ctx->LastResponse) ? n : sizeof(ctx->LastResponse);
            RtlCopyMemory(o, ctx->LastResponse, copyN);
            o[0] = 0x01;
            transferred = copyN;
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }

    default:
        status = STATUS_NOT_SUPPORTED;
        break;
    }

    WdfRequestSetInformation(Request, transferred);
    WdfRequestComplete(Request, status);
}
