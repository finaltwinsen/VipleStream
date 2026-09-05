#pragma once

#include <SDL_hidapi.h>   // SDL_hid_device + SDLCALL (via begin_code.h)
#include <SDL_thread.h>   // SDL_Thread + SDL_ThreadFunction
#include <SDL_stdinc.h>   // Uint32

#include <atomic>   // §SC-DEV-LOCK-FIX (review batch 2)
#include <cstdint>
#include <mutex>    // §SC-DEV-LOCK-FIX (review batch 2)

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
// forwardFeatureRequest 改成 SET 後每 1 ms 輪詢 GET，並以 type 比對回應。

class ScHidPassthrough {
public:
    ScHidPassthrough();
    ~ScHidPassthrough();

    // Call once after LiStartConnection() succeeds.
    // Scans for a connected Steam Controller and starts the read thread.
    // Safe to call even if no Steam Controller is present (no-op).
    void start();

    // Call once before LiStopConnection(). Stops the read thread and
    // closes the HID device.
    void stop();

    bool isActive() const;

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
    // The real SC's response is returned to the host via LiSendScHidFeatureReport
    // tagged with the same seq.
    void forwardFeatureRequest(uint8_t reportId, uint8_t op, uint8_t seq,
                               const uint8_t* query, uint8_t queryLen);

private:
    static int SDLCALL readThreadFunc(void* ctx);
    void readLoop();

    // A Steam Controller (especially via the Puck receiver) exposes SEVERAL
    // vendor interfaces (idle slots + the active controller). We open them all
    // and poll each non-blocking, forwarding from whichever actually streams —
    // so direct/Puck/Bluetooth connections are all covered without guessing.
    static constexpr int MAX_SC_DEVS = 8;
    static constexpr int SC_HID_WIRE_BYTES = 64;

    SDL_hid_device* m_devs[MAX_SC_DEVS] = {};
    // 列舉時記下的識別資訊（只供 log；start() 鎖內填、之後唯讀）
    unsigned short m_devPid[MAX_SC_DEVS] = {};
    int m_devIf[MAX_SC_DEVS] = {};
    // 0x46/0x79 無線狀態：0 未知 / 1 disconnect / 2 connect（Valve ETritonWirelessState）
    uint8_t m_devLink[MAX_SC_DEVS] = {};

    // §SC-DEV-LOCK-FIX：atomic——isActive() 在 SDL 事件執行緒無鎖讀，
    // 與 start()/stop() 鎖內寫並發；其餘存取都在 m_devMutex 內。
    std::atomic<int> m_devCount{0};
    SDL_Thread* m_thread = nullptr;
    std::atomic<bool> m_running{false};

    // §SC-HID Round 1：最近吐 gamepad state（0x42/0x45）的 slot；-1 = 尚未看到。
    // readLoop 寫、forwardFeatureRequest 讀（都在鎖內），atomic 只是讓 log /
    // 未來無鎖讀者安全。
    std::atomic<int> m_activeDev{-1};

    // §SC-DEV-LOCK-FIX (review batch 2)：保護 m_devs/m_devCount 與所有
    // hidapi 呼叫。readLoop（SC-HID 執行緒）、forwardFeatureRequest
    // （control stream async callback 執行緒）與 start()/stop() 會碰同
    // 一組 handle；hidapi 同 handle 跨執行緒並發是 UB。
    // 以下所有計數器 / 快取 / seen mask 也只在 m_devMutex 內讀寫。
    std::mutex m_devMutex;

    // ── 讀取統計（每場 start() 歸零；取代舊的 function-static seenMask）──
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
    uint32_t m_featReq = 0;             // forwardFeatureRequest 次數（含暖機）
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

    // 處理一筆讀到的 input report（鎖內）；回傳是否有轉發給 host
    bool handleInputReport(int dev, uint8_t* buf, int n);
    // 一次 feature 請求的完整流程（鎖內；start() 暖機與 forwardFeatureRequest 共用）。
    // pollBudgetMs = SET 的總預算（從進入函式起算、含送 SET 的時間；硬上限）。
    // 回傳 2 = 即時回應、1 = 以快取回覆、0 = 空（空回應一律不送 host，只印 warning）。
    int featureRequestLocked(uint8_t reportId, uint8_t op, uint8_t seq,
                             const uint8_t* query, int queryLen,
                             Uint32 pollBudgetMs, const char* tag);
    // 在 devs[] 上輪詢 GET_FEATURE(reportId) 直到拿到 type 相符的回應或逾時（鎖內）。
    // 逾時檢查在 dev 迴圈內（每次 GET 前）= 硬上限；type 不符的回應累加 *stolen，
    // 每次請求最多印 1 行（*stolen 由呼叫者每請求歸零）。
    // 回傳 >0 = resp 已填（byte0 = reportId）並設 *respDev；0 = 逾時。
    int pollFeatureResponse(const int* devs, int ndevs, uint8_t reportId, uint8_t expectType,
                            Uint32 timeoutMs, uint8_t* resp, int* respDev, uint32_t* stolen);
    // 印一行統計（鎖內）
    void logRxStats(const char* tag);
    void resetStats();
};
