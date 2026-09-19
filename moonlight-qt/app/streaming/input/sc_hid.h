#pragma once

#include <SDL_hidapi.h>   // SDL_hid_device + SDLCALL (via begin_code.h)
#include <SDL_thread.h>   // SDL_Thread + SDL_ThreadFunction
#include <SDL_stdinc.h>   // Uint32

#include <atomic>
#include <cstdint>
#include <mutex>    // feature 請求佇列（唯一的鎖；絕不在 hidapi 呼叫期間持有）

// §SC-HID: Steam Controller raw-HID passthrough (client side)
// Manages a background thread that reads raw HID input reports from a locally
// connected gen-2 Steam Controller — selected by its vendor interface
// (VID 0x28DE, UsagePage 0xFF00 / Usage 0x01), so USB-direct (0x1302), Puck
// (0x1304) and Bluetooth (0x1303) are all handled — and forwards each gamepad
// state report (Valve id 0x42 / 0x45) to the host via LiSendScHidInputReport().
//
// §SC-HID Round 1（2026-09-02）：Puck 走的是 0x42（ID_TRITON_CONTROLLER_STATE），
// 與 0x45 同 TritonMTUNoQuat_t 佈局；client 端正規化成 0x45 再送 host。feature
// 回應在實體 SC 是「SET 後 13~21 ms 出現、讀一次即清」的一次性暫存器，所以
// feature 請求改成 SET 後每 1 ms 輪詢 GET，並以 type 比對回應。
//
// §SC-THREAD-OWNER（2026-09-19，取代 §SC-DEV-LOCK-FIX 的 m_devMutex）：
// **所有 hidapi 呼叫（enumerate 之外）只在 SC-HID 讀取執行緒上發生。** 其他執行緒只碰
// 一個小佇列：forwardFeatureRequest（common-c control stream 的 async callback 執行緒）
// 把請求排進去就返回，由讀取執行緒代跑。理由：HidD_SetFeature／GetFeature 是同步
// IOCTL，裝置一旦卡死（2026-09-19 Puck 事故）呼叫端會在核心永久阻塞、使用者態拉不
// 回來；以前 forwardFeatureRequest 在 m_devMutex 內卡住 → readLoop 搶不到鎖 →
// stop() 的 SDL_WaitThread 永不返回 → session 收尾卡死；更糟的是卡住的是 common-c
// 的執行緒，LiStopConnection 也會等它。現在只有我們自己的執行緒會卡，stop() 等
// kStopJoinTimeoutMs 不回來就 detach + 遺棄 handle（不 close：close 會把第二條執行緒
// 也賠進去），session 照常收尾；下一場 validateLaunch 的 HidProbe::refresh() 會再次
// 抓到卡死裝置並拒絕啟動。

struct ScHidThreadCtx;   // 讀取執行緒的 context（sc_hid.cpp 內定義；由執行緒自己擁有）

class ScHidPassthrough {
public:
    ScHidPassthrough();
    ~ScHidPassthrough();

    // 階段 1（主執行緒，Session::start() 在建 SdlInputHandler 之前呼叫）：
    // 列舉並開啟 Steam Controller 的 vendor 介面，不碰連線、不送任何 feature。
    // 回傳是否開到至少一個介面；SdlInputHandler 據此把 SC 家族加進 SDL 的
    // 忽略清單，讓 SDL 的 hidapi 驅動（SDL 3.4.x SDL_hidapi_steam_triton）連開都不開，
    // 不再與 passthrough 搶同一組 feature 暫存器。沒有 SC 時 no-op。
    bool prepare();

    // 階段 2：LiStartConnection() 成功後呼叫。排入暖機查詢並啟動讀取執行緒。
    // 沒 prepare 到裝置時 no-op。
    void start();

    // Call once before LiStopConnection(). 停讀取執行緒（逾時則遺棄）並關閉裝置。
    // 只 prepare 沒 start 的情況也會把 handle 關掉。
    void stop();

    bool isActive() const;
    bool isPrepared() const { return m_devCount.load() > 0; }

    // SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES 用的 "0xVID/0xPID,…" 清單：gen-2 SC 全家族
    // （1302 USB／1303 BLE／1304 Puck／1305 Nereid）。SdlInputHandler 在 isPrepared() 時併入。
    static const char* sdlIgnoreDevicesSpec();

    // §SC-HID Phase 2C: transparent feature proxy. The host relays a Steam
    // feature op against the real SC:
    //   op==2 (SET): SDL_hid_send_feature_report(query)，再每 1 ms
    //                SDL_hid_get_feature_report(reportId) 輪詢直到拿到
    //                type 相符的回應（最多 100 ms；逾時退回 5 s 內同 query 的快取）
    //   op==1 (GET): 只有本場已 SET 過、且距最近一次 SET < 400 ms 才輪詢
    //                （≤ 10 ms）；否則不輪詢，直接退回 5 s 內同 type 的快取
    //   拿不到（無即時回應、無可用快取）→ **一律不回零、不呼叫
    //   LiSendScHidFeatureReport**：host driver 的 GET 閘控 400 ms 逾時會自己
    //   退回 LastResponse；回零反而把 LastResponse 蓋成零。
    // §SC-THREAD-OWNER：本函式只把請求排進佇列（≤ 1 ms 後由讀取執行緒執行），
    // 呼叫端絕不會碰到 hidapi、絕不會被卡死的裝置絆住。
    void forwardFeatureRequest(uint8_t reportId, uint8_t op, uint8_t seq,
                               const uint8_t* query, uint8_t queryLen);

private:
    static int SDLCALL readThreadFunc(void* ctx);
    void readLoop(ScHidThreadCtx* ctx);

    // A Steam Controller (especially via the Puck receiver) exposes SEVERAL
    // vendor interfaces (idle slots + the active controller). We open them all
    // and poll each non-blocking, forwarding from whichever actually streams —
    // so direct/Puck/Bluetooth connections are all covered without guessing.
    static constexpr int MAX_SC_DEVS = 8;
    static constexpr int SC_HID_WIRE_BYTES = 64;

    // 讀取執行緒等待上限：feature 請求鎖內最長 100 ms（kFeatSetPollMs）＋ SET 本身，
    // 健康裝置 ≤ ~150 ms 必定退出；超過就是卡在核心。
    static constexpr Uint32 kStopJoinTimeoutMs = 1000;

    SDL_hid_device* m_devs[MAX_SC_DEVS] = {};
    // 列舉時記下的識別資訊（只供 log；prepare() 填、之後唯讀）
    unsigned short m_devPid[MAX_SC_DEVS] = {};
    int m_devIf[MAX_SC_DEVS] = {};
    // 0x46/0x79 無線狀態：0 未知 / 1 disconnect / 2 connect（Valve ETritonWirelessState）
    uint8_t m_devLink[MAX_SC_DEVS] = {};

    // atomic——isActive()/isPrepared() 在其他執行緒無鎖讀，prepare()/stop() 寫。
    std::atomic<int> m_devCount{0};
    SDL_Thread* m_thread = nullptr;
    ScHidThreadCtx* m_threadCtx = nullptr;      // 正常 join 後 stop() 釋放；遺棄時連同執行緒一起漏掉
    std::atomic<bool> m_running{false};
    // 世代：每次 prepare() +1。被遺棄的執行緒醒來後比對世代不符就退出，不碰任何成員。
    std::atomic<uint32_t> m_gen{0};
    bool m_abandoned = false;                   // 上一場 stop() 逾時遺棄了執行緒＋handle（只供 log）

    // 讀取執行緒目前／最後一次進入的 hidapi 呼叫（stop() 逾時時印出卡在哪一步）
    enum LastOp : int { OpIdle = 0, OpRead = 1, OpSetFeature = 2, OpGetFeature = 3 };
    std::atomic<int> m_lastOp{OpIdle};
    std::atomic<Uint32> m_lastOpTick{0};

    // §SC-HID Round 1：最近吐 gamepad state（0x42/0x45）的 slot；-1 = 尚未看到。
    // readLoop 讀寫；atomic 只是讓 log／無鎖讀者安全。
    std::atomic<int> m_activeDev{-1};

    // ── feature 請求佇列（forwardFeatureRequest 生產、readLoop 消費）──
    struct FeatureReq {
        uint8_t reportId;
        uint8_t op;
        uint8_t seq;
        uint8_t queryLen;
        uint8_t query[SC_HID_WIRE_BYTES];
        Uint32 budgetMs;
        const char* tag;       // 字串常量（" (warm-up)" 或 ""）
    };
    static constexpr int FEAT_QUEUE_LEN = 8;
    std::mutex m_queueMutex;   // 只保護下面四個欄位；持有期間不呼叫任何 hidapi
    FeatureReq m_queue[FEAT_QUEUE_LEN] = {};
    int m_queueHead = 0;
    int m_queueCount = 0;
    uint32_t m_queueDropped = 0;
    void enqueueFeatureRequest(const FeatureReq& req);
    bool dequeueFeatureRequest(FeatureReq* out);

    // ── 讀取統計（每場 prepare() 歸零）：只在讀取執行緒（與 join 之後的 stop()）存取 ──
    uint32_t m_seenMask[8] = {};        // report id 0..255 首見 bitmap
    uint32_t m_rxTotal = 0;             // 所有 dev 讀到的 report 數
    uint32_t m_rxFwd = 0;               // LiSendScHidInputReport 回 0 的筆數
    uint32_t m_rxNorm42 = 0;            // 0x42 改標 0x45 後轉發的筆數
    uint32_t m_rxDrop = 0;              // 非 gamepad state、未轉發的筆數
    uint32_t m_rxSendErr = 0;           // LiSendScHidInputReport 非 0 回傳
    uint32_t m_rxId42 = 0, m_rxId45 = 0, m_rxId43 = 0, m_rxId7B = 0, m_rxId47 = 0, m_rxIdOther = 0;
    uint32_t m_devRx[MAX_SC_DEVS] = {};
    uint32_t m_devFwd[MAX_SC_DEVS] = {};

    // ── feature proxy 統計 ──
    uint32_t m_featReq = 0;             // 執行的 feature 請求次數（含暖機）
    uint32_t m_featOk = 0;              // 拿到 type 相符的即時回應
    uint32_t m_featStolen = 0;          // 輪詢撿到 type 不符的回應（如未請求的 0x87 ack）
    uint32_t m_featEmpty = 0;           // 逾時沒回應（含退回快取 / 回零）
    uint32_t m_featCache = 0;           // 以快取回覆的次數
    uint32_t m_featLatSumMs = 0;        // 即時回應延遲總和（算平均）
    uint32_t m_featLatMaxMs = 0;

    // ── 最近一筆即時 feature 回應快取（host Steam 重讀 GET / SET 逾時退回用）──
    bool m_featCacheValid = false;
    Uint32 m_featCacheTick = 0;         // SDL_GetTicks() 當時
    uint8_t m_featCacheType = 0;        // 回應 byte1（Valve FeatureReportHeader.type）
    // 觸發它的完整 SET query（零填充到 64 bytes；SET 退回時 memcmp 整段）。
    // GET op 的即時回應更新快取時會清零——那筆回應不對應任何一筆我們送的 query，
    // 清零後 byte0=0 ≠ reportId，SET 退回必不配對。
    uint8_t m_featCacheQuery[SC_HID_WIRE_BYTES] = {};
    uint8_t m_featCacheResp[SC_HID_WIRE_BYTES] = {};
    uint8_t m_lastSetType = 0;          // 最近一次 SET 的 query type（GET op 的比對依據；0 = 本場尚未 SET）
    Uint32 m_lastSetTick = 0;           // 最近一次 SET 的 SDL_GetTicks()（GET op 只在 400 ms 視窗內才輪詢）

    // 處理一筆讀到的 input report（讀取執行緒）；回傳是否有轉發給 host
    bool handleInputReport(int dev, uint8_t* buf, int n);
    // 一次 feature 請求的完整流程（讀取執行緒；暖機與 host 請求共用）。
    // pollBudgetMs = SET 的總預算（從進入函式起算、含送 SET 的時間；硬上限）。
    // 回傳 2 = 即時回應、1 = 以快取回覆、0 = 空（空回應一律不送 host，只印 warning）。
    int runFeatureRequest(uint8_t reportId, uint8_t op, uint8_t seq,
                          const uint8_t* query, int queryLen,
                          Uint32 pollBudgetMs, const char* tag);
    // 在 devs[] 上輪詢 GET_FEATURE(reportId) 直到拿到 type 相符的回應或逾時（讀取執行緒）。
    // 逾時檢查在 dev 迴圈內（每次 GET 前）= 硬上限；type 不符的回應累加 *stolen，
    // 每次請求最多印 1 行（*stolen 由呼叫者每請求歸零）。stop() 進行中（m_running=false）
    // 也提前結束。回傳 >0 = resp 已填（byte0 = reportId）並設 *respDev；0 = 逾時。
    int pollFeatureResponse(const int* devs, int ndevs, uint8_t reportId, uint8_t expectType,
                            Uint32 timeoutMs, uint8_t* resp, int* respDev, uint32_t* stolen);
    // 印一行統計（讀取執行緒）
    void logRxStats(const char* tag);
    void resetStats();
    void closeDevices();   // 關閉 m_devs[]（只能在沒有執行緒、或執行緒已 join 之後呼叫）
};
