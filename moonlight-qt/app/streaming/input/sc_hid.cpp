// §SC-HID: Steam Controller raw-HID passthrough (client side)

#include "sc_hid.h"
#include "SDL_compat.h"
#include "backend/hidprobe.h"   // §HID-PROBE
#include <SDL_hidapi.h>
#include <cstdio>    // snprintf
#include <cstring>   // memset / memcpy / memcmp

extern "C" {
#include <Limelight.h>
}

// Wire report size：SS_SC_HID_REPORT_MAX（moonlight-common-c 內部 Input.h，不在
// app include path）。LiSendScHidInputReport / LiSendScHidFeatureReport 一律送
// 剛好 ScHidPassthrough::SC_HID_WIRE_BYTES（64）bytes。

// Valve vendor id. The gen-2 (2025) Steam Controller exposes its gamepad data
// on a vendor-defined HID interface (UsagePage 0xFF00 / Usage 0x01). We select
// the device by that usage rather than by a hard-coded product id, so all
// transports are handled by one code path:
//   0x1302 = USB-direct, 0x1303 = Bluetooth LE, 0x1304 = Puck (wireless).
static constexpr unsigned short SC_VID        = 0x28DE;
static constexpr unsigned short SC_USAGE_PAGE = 0xFF00;
static constexpr unsigned short SC_USAGE      = 0x0001;

// gen-2 Steam Controller PID 家族（SDL usb_ids.h / controller_list.h）：
//   0x1302 USB 直連、0x1303 Bluetooth LE、0x1304 Puck（Proteus dongle）、0x1305 Nereid dongle。
static constexpr bool isGen2Pid(unsigned short pid) {
    return pid == 0x1302 || pid == 0x1303 || pid == 0x1304 || pid == 0x1305;
}

// SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES 格式："0xVID/0xPID,…"（SDL 以 strtoul 解析，大小寫不拘）。
// 與上面的 isGen2Pid 同一份家族清單；新增 PID 時兩處一起改。
static constexpr char kSdlIgnoreDevicesSpec[] = "0x28DE/0x1302,0x28DE/0x1303,0x28DE/0x1304,0x28DE/0x1305";

// ── Valve 官方 input report id（temp\sc-hid-research\controller_structs.h 的
// ETritonReportIDTypes）。SDL_hidapi_steam_triton.c 對 0x42 與 0x45 走同一個
// case、同 TritonMTUNoQuat_t 佈局；0x47 是 TritonMTUNoQuat32TS_t（佈局不同，
// 不能只改 id）。實測（2026-09-02，F11）：Puck slot 以 ~266 Hz 吐 0x42（54 bytes），
// 完全沒有 0x45 / 0x47；0x43 電量 ~0.3/s、0x7B 遙測 ~1.5/s、0x44 偶發。
static constexpr uint8_t SC_RID_STATE      = 0x42;  // ID_TRITON_CONTROLLER_STATE：USB 直連 / Puck 的 gamepad state
static constexpr uint8_t SC_RID_BATTERY    = 0x43;  // ID_TRITON_BATTERY_STATUS
static constexpr uint8_t SC_RID_STATE_BLE  = 0x45;  // ID_TRITON_CONTROLLER_STATE_BLE：BLE 的 gamepad state；也是 host 虛擬裝置給 Steam 解析的 input id
static constexpr uint8_t SC_RID_WIRELESS_X = 0x46;  // ID_TRITON_WIRELESS_STATUS_X（byte1 = ETritonWirelessState）
static constexpr uint8_t SC_RID_STATE_TS   = 0x47;  // ID_TRITON_CONTROLLER_STATE_TIMESTAMP（TritonMTUNoQuat32TS_t，佈局不同）
static constexpr uint8_t SC_RID_WIRELESS   = 0x79;  // ID_TRITON_WIRELESS_STATUS（byte1 = ETritonWirelessState）
static constexpr uint8_t SC_RID_TELEMETRY  = 0x7B;  // 實測 Puck slot 遙測（非 Valve 公開定義；不含 gamepad state）
static constexpr uint8_t SC_WIRELESS_DISCONNECT = 1; // k_ETritonWirelessStateDisconnect
static constexpr uint8_t SC_WIRELESS_CONNECT    = 2; // k_ETritonWirelessStateConnect

// ── feature 通道（Valve FeatureReportHeader{type,length} 接在 report id 0x01 之後）──
static constexpr uint8_t SC_FEATURE_RID = 0x01;
static constexpr uint8_t SC_MSG_GET_ATTRIBUTES_VALUES = 0x83;  // controller_constants.h ID_GET_ATTRIBUTES_VALUES
static constexpr uint8_t SC_OP_GET = 1;   // host 送來的 op：Steam 對虛擬裝置做了 GET_FEATURE
static constexpr uint8_t SC_OP_SET = 2;   // host 送來的 op：Steam 對虛擬裝置做了 SET_FEATURE（query 隨附）

// 開關（hardcode，不用 env var）：0x42 改標成 0x45 再送 host。host 虛擬裝置目前把
// 0x45 當 gamepad state 交給 Steam 解析，而兩者佈局相同（SDL 證實）。設 false 即
// 原樣透傳 0x42，走 host driver 宣告的 0x42 Input 通道（Plan B）。
static constexpr bool kNormalize42 = true;

// ── feature 時序（F13 實測）：SET 寫進 0x01 後，回應在 13~21 ms 內出現在同一個
// 一次性暫存器，GET 讀到即清空；其他時間 GET 一律 ERROR_GEN_FAILURE(31)。舊做法
// 「SET → 等 20 ms → 單次 GET」必定錯過，所以改成 SET 後每 1 ms 輪詢、上限 100 ms。
// 對應 host driver（VipleSCHid_Driver.cpp）SC_RESPONSE_PENDING_TIMEOUT_MS = 400：
// Steam 對虛擬裝置的 GET 在 SET 後最多被閘控 400 ms，逾時 driver 自己退回 LastResponse。
// client 端 100 ms 輪詢 + 兩段控制流往返仍在那個視窗內；拿不到就「不回覆」，交給
// driver 逾時退回（回零會把 LastResponse 蓋成零，比不回更糟；driver 也會忽略全零
// 的 WRITE(0x04)，所以回零只是白白多一趟）。
static constexpr Uint32 kFeatSetPollMs      = 100;  // SET 後輪詢上限（執行緒內最長時間；< driver 400 ms 閘控）
static constexpr Uint32 kFeatGetPollMs      = 10;   // host Steam 重讀（GET op）的輪詢上限
static constexpr Uint32 kFeatGetWindowMs    = 400;  // GET op 只在距最近一次 SET 這麼近時才輪詢（= driver 閘控逾時）
static constexpr Uint32 kFeatWarmupBudgetMs = 60;   // 暖機查詢的總預算（含送 SET；硬上限）
static constexpr Uint32 kFeatCacheTtlMs     = 5000; // 退回快取的有效期
static constexpr Uint32 kFeatPollStepMs     = 1;

// ── 統計 log 節奏 ──
static constexpr Uint32 kStatsPeriodMs  = 5000;   // 有變才印
static constexpr Uint32 kHeartbeatMs    = 60000;  // 沒變也印一次
static constexpr Uint32 kNoPacketWarnMs = 3000;   // 啟動後仍 total=0 → warning 一次
static constexpr Uint32 kStopPollStepMs = 2;      // stop() 等執行緒退出的輪詢間隔

// §SC-THREAD-OWNER：讀取執行緒的 context 由執行緒自己擁有。正常 join 後由 stop() 釋放；
// 逾時遺棄時連同 context 一起漏掉——卡在核心的執行緒醒來後只會讀 self->m_running /
// m_gen（atomic）發現世代不符就退出，不再碰任何成員（見 readLoop 的 alive()）。
struct ScHidThreadCtx {
    ScHidPassthrough* self;
    uint32_t gen;
    std::atomic<bool> exited{false};
    bool simulateStall;   // dev-only：VIPLE_SC_HID_SIMULATE_STALL=1 → 收到 stop 後假裝卡死，驗證遺棄路徑
};

static const char* opName(uint8_t op) {
    return op == SC_OP_SET ? "SET" : (op == SC_OP_GET ? "GET" : "?");
}

static const char* lastOpName(int op) {
    switch (op) {
    case 1: return "SDL_hid_read";
    case 2: return "SDL_hid_send_feature_report (HidD_SetFeature)";
    case 3: return "SDL_hid_get_feature_report (HidD_GetFeature)";
    default: return "idle (between calls)";
    }
}

static const char* wirelessName(uint8_t state) {
    return state == SC_WIRELESS_CONNECT ? "connect"
         : (state == SC_WIRELESS_DISCONNECT ? "disconnect" : "unknown");
}

// bytes → "42 00 01 …"（out 至少 n*3+1）
static void hexDump(char* out, size_t outSz, const uint8_t* b, int n) {
    size_t pos = 0;
    if (outSz == 0) return;
    out[0] = '\0';
    for (int i = 0; i < n && pos + 4 <= outSz; i++) {
        int w = snprintf(out + pos, outSz - pos, i ? " %02x" : "%02x", b[i]);
        if (w < 0) break;
        pos += (size_t)w;
    }
}

ScHidPassthrough::ScHidPassthrough() = default;

ScHidPassthrough::~ScHidPassthrough() {
    stop();
}

const char* ScHidPassthrough::sdlIgnoreDevicesSpec() {
    return kSdlIgnoreDevicesSpec;
}

void ScHidPassthrough::resetStats() {
    memset(m_seenMask, 0, sizeof(m_seenMask));
    m_rxTotal = m_rxFwd = m_rxNorm42 = m_rxDrop = m_rxSendErr = 0;
    m_rxId42 = m_rxId45 = m_rxId43 = m_rxId7B = m_rxId47 = m_rxIdOther = 0;
    memset(m_devRx, 0, sizeof(m_devRx));
    memset(m_devFwd, 0, sizeof(m_devFwd));
    memset(m_devPid, 0, sizeof(m_devPid));
    memset(m_devIf, 0, sizeof(m_devIf));
    memset(m_devLink, 0, sizeof(m_devLink));
    m_featReq = m_featOk = m_featStolen = m_featEmpty = m_featCache = 0;
    m_featLatSumMs = m_featLatMaxMs = 0;
    m_featCacheValid = false;
    m_featCacheTick = 0;
    m_featCacheType = 0;
    memset(m_featCacheQuery, 0, sizeof(m_featCacheQuery));
    memset(m_featCacheResp, 0, sizeof(m_featCacheResp));
    m_lastSetType = 0;
    m_lastSetTick = 0;
    m_activeDev = -1;
    m_lastOp = OpIdle;
    m_lastOpTick = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queueHead = 0;
        m_queueCount = 0;
        m_queueDropped = 0;
    }
}

void ScHidPassthrough::closeDevices() {
    const int count = m_devCount.load();
    for (int i = 0; i < count; i++) {
        if (m_devs[i]) {
            SDL_hid_close(m_devs[i]);
            m_devs[i] = nullptr;
        }
    }
    m_devCount = 0;
    m_activeDev = -1;
}

void ScHidPassthrough::logRxStats(const char* tag) {
    // 每個 dev 的識別（pid/if，prepare() 列舉時記下）與 rx/fwd——
    // final 那行和 host 的 write stats 對帳時，才看得出是哪個 slot 在吐資料。
    char devPart[MAX_SC_DEVS * 48 + 1];
    devPart[0] = '\0';
    size_t pos = 0;
    const int count = m_devCount.load();
    for (int i = 0; i < count && pos + 1 < sizeof(devPart); i++) {
        int w = snprintf(devPart + pos, sizeof(devPart) - pos, "%sdev%d[%04x/%d]=%u/%u",
                         i ? " " : "", i, m_devPid[i], m_devIf[i], m_devRx[i], m_devFwd[i]);
        if (w < 0) break;
        pos += (size_t)w;
    }
    const uint32_t latAvg = m_featOk ? (m_featLatSumMs / m_featOk) : 0;
    uint32_t dropped;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        dropped = m_queueDropped;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SC-HID] rx stats(%s): total=%u fwd=%u norm42=%u drop=%u sendErr=%u | "
        "id42=%u id45=%u id43=%u id7B=%u id47=%u other=%u | "
        "feat req=%u ok=%u stolen=%u empty=%u cache=%u qdrop=%u lat avg/max=%u/%u ms | "
        "active=%d | dev[pid/if] rx/fwd: %s",
        tag, m_rxTotal, m_rxFwd, m_rxNorm42, m_rxDrop, m_rxSendErr,
        m_rxId42, m_rxId45, m_rxId43, m_rxId7B, m_rxId47, m_rxIdOther,
        m_featReq, m_featOk, m_featStolen, m_featEmpty, m_featCache, dropped, latAvg, m_featLatMaxMs,
        m_activeDev.load(), devPart);
}

// ── feature 請求佇列 ──
void ScHidPassthrough::enqueueFeatureRequest(const FeatureReq& req) {
    uint32_t droppedNow = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_queueCount == FEAT_QUEUE_LEN) {
            // 滿了丟最舊的：host driver 的 400 ms 閘控會退回 LastResponse，舊請求早已無意義
            m_queueHead = (m_queueHead + 1) % FEAT_QUEUE_LEN;
            m_queueCount--;
            m_queueDropped++;
            droppedNow = m_queueDropped;
        }
        m_queue[(m_queueHead + m_queueCount) % FEAT_QUEUE_LEN] = req;
        m_queueCount++;
    }
    if (droppedNow == 1 || (droppedNow != 0 && (droppedNow % 32) == 0)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] feature queue full (%d) - dropped oldest request (dropped=%u); "
            "is the read thread stuck?", FEAT_QUEUE_LEN, droppedNow);
    }
}

bool ScHidPassthrough::dequeueFeatureRequest(FeatureReq* out) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_queueCount == 0) return false;
    *out = m_queue[m_queueHead];
    m_queueHead = (m_queueHead + 1) % FEAT_QUEUE_LEN;
    m_queueCount--;
    return true;
}

// 在 devs[] 上輪詢 GET_FEATURE(reportId)，直到拿到 type 相符（resp[1] == expectType；
// expectType==0 表示不比對）的回應或逾時。讀取執行緒專用。
// 一次性暫存器語義：讀到 type 不符的回應（本機 Steam 的查詢、dongle 未經請求的
// 0x87 ack）代表它已被我們讀掉，計 stolen 後繼續等我們自己的那筆。
// 逾時檢查放在 dev 迴圈內（每次 GET 之前）＝硬上限：多個 slot 一起輪時也不會因為
// 「整輪 sweep 跑完才檢查」而超出 timeoutMs。m_running 一變 false（stop() 進行中）也立刻
// 收手——每個 hidapi 呼叫前都檢查，卡死的呼叫醒來後不會再發下一個。
// 「unexpected type」每次請求最多印 1 行（*stolen 由呼叫者每請求歸零），其餘只累加——
// 本機 Steam 常駐時 stolen 可能一連串出現，不要洗 log；總數在 Feature req 那行的 stolen=。
int ScHidPassthrough::pollFeatureResponse(const int* devs, int ndevs, uint8_t reportId,
                                          uint8_t expectType, Uint32 timeoutMs,
                                          uint8_t* resp, int* respDev, uint32_t* stolen) {
    if (ndevs <= 0) return 0;   // 防禦：沒有候選就不進迴圈（否則 for(;;) 永不結束）
    const Uint32 t0 = SDL_GetTicks();
    for (;;) {
        for (int k = 0; k < ndevs; k++) {
            if (!m_running || SDL_GetTicks() - t0 >= timeoutMs) {
                return 0;
            }
            const int d = devs[k];
            memset(resp, 0, SC_HID_WIRE_BYTES);
            resp[0] = reportId;
            m_lastOp = OpGetFeature;
            m_lastOpTick = SDL_GetTicks();
            const int n = SDL_hid_get_feature_report(m_devs[d], resp, SC_HID_WIRE_BYTES);
            m_lastOp = OpIdle;
            if (n <= 0) {
                continue;  // err 31 = 暫存器目前是空的（正常），下一輪再讀
            }
            if (expectType == 0 || resp[1] == expectType) {
                if (n < SC_HID_WIRE_BYTES) memset(resp + n, 0, SC_HID_WIRE_BYTES - n);
                resp[0] = reportId;
                *respDev = d;
                return n;
            }
            (*stolen)++;
            if (*stolen == 1) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SC-HID] Feature poll: unexpected type=0x%02x (want 0x%02x) on dev=%d "
                    "(%d bytes) — discarded, keep polling (further ones only counted)",
                    resp[1], expectType, d, n);
            }
        }
        SDL_Delay(kFeatPollStepMs);
    }
}

// 一次 feature 請求的完整流程（讀取執行緒專用，m_devCount > 0）：
//   SET：query 送到候選 dev，再每 1 ms 輪詢 GET 直到 type 相符或 pollBudgetMs 用完
//        （預算從進入本函式起算、含送 SET 的時間——硬上限）；逾時退回 kFeatCacheTtlMs
//        內「整段 query 相同」的快取。
//   GET（host Steam 重讀）：只有本場已 SET 過（m_lastSetType != 0）且距最近一次 SET
//        < kFeatGetWindowMs 才輪詢（≤ kFeatGetPollMs）；否則不輪詢，直接看快取（同 type）。
//        本場沒 SET 過 → 不輪詢、不接受任何回應（含快取），直接 empty。
//   空回應（無即時回應、無可用快取）一律不送 host：driver 端 GET 閘控 400 ms 逾時會
//   自己退回 LastResponse；回零反而把 LastResponse 蓋成零、讓 Steam 拿到假資料
//   （driver 現在也會忽略全零 WRITE，回零只是白跑一趟）。
// 回傳 2 = 即時回應、1 = 以快取回覆、0 = 空（未回覆 host）。
// 每次請求印一行 `[SC-HID] Feature req …`（空回應以 warning 印）。
int ScHidPassthrough::runFeatureRequest(uint8_t reportId, uint8_t op, uint8_t seq,
                                        const uint8_t* query, int queryLen,
                                        Uint32 pollBudgetMs, const char* tag) {
    const Uint32 t0 = SDL_GetTicks();
    m_featReq++;

    // 組 query：host 送來的 query 本來就含 byte0 = report id（driver push 的是完整
    // report），這裡再以 reportId 覆寫一次保證正確。qbuf 零填充到 64 bytes，快取比對
    // 直接 memcmp 整段（長度差異自然包含在內）。
    uint8_t qbuf[SC_HID_WIRE_BYTES] = {};
    int qn = queryLen;
    if (qn > SC_HID_WIRE_BYTES) qn = SC_HID_WIRE_BYTES;
    if (qn < 0) qn = 0;
    if (query && qn > 0) memcpy(qbuf, query, (size_t)qn);
    qbuf[0] = reportId;

    // 比對用 type：SET 用 query 的 FeatureReportHeader.type（byte1）；GET（Steam 重讀）
    // 沒有 query，用最近一次 SET 的 type。expectType==0 在 SET 表示不比對（無 type 的
    // 畸形 query，實務上 driver 推的是完整 report，不會發生）；在 GET 表示本場還沒
    // SET 過 → 不輪詢、不接受任何回應。
    uint8_t expectType = 0;
    if (op == SC_OP_SET && qn >= 2) {
        expectType = qbuf[1];
    } else if (op == SC_OP_GET) {
        expectType = m_lastSetType;
    }

    // 候選 dev：最近吐 gamepad state 的 slot 優先（m_activeDev）；還不知道就全部一起
    // 輪——SET 先送到每個候選，再統一輪詢，讓每個 slot 都有完整視窗而總時間
    // 仍 ≤ pollBudgetMs。
    int cand[MAX_SC_DEVS];
    int nc = 0;
    const int active = m_activeDev.load();
    const int count = m_devCount.load();
    if (active >= 0 && active < count) {
        cand[nc++] = active;
    } else {
        for (int i = 0; i < count; i++) cand[nc++] = i;
    }

    uint8_t resp[SC_HID_WIRE_BYTES] = {};
    int respDev = -1;
    int n = 0;
    uint32_t stolen = 0;
    int sentCount = 0;
    Uint32 pollMs = 0;   // 這次實際給輪詢的上限（0 = 沒輪詢；log 用）

    if (op == SC_OP_SET) {
        m_lastSetType = expectType;
        m_lastSetTick = t0;
        int sent[MAX_SC_DEVS];
        for (int k = 0; k < nc; k++) {
            if (!m_running) break;   // stop() 進行中：不再發下一個同步 IOCTL
            const int d = cand[k];
            m_lastOp = OpSetFeature;
            m_lastOpTick = SDL_GetTicks();
            const int sn = SDL_hid_send_feature_report(m_devs[d], qbuf, sizeof(qbuf));
            m_lastOp = OpIdle;
            if (sn < 0) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SC-HID] Feature SET id=0x%02x type=0x%02x failed on dev=%d: %s",
                    reportId, expectType, d, SDL_GetError());
                continue;
            }
            sent[sentCount++] = d;
        }
        if (sentCount > 0) {
            // 硬上限：預算扣掉送 SET 已花的時間（暖機 60 ms、線上 100 ms 都含送出）
            const Uint32 spent = SDL_GetTicks() - t0;
            pollMs = pollBudgetMs > spent ? (pollBudgetMs - spent) : 0;
            n = pollFeatureResponse(sent, sentCount, reportId, expectType, pollMs,
                                    resp, &respDev, &stolen);
        }
    } else if (expectType != 0 && (t0 - m_lastSetTick) < kFeatGetWindowMs) {
        // 最近一次 SET 仍在 driver 的閘控視窗內：回應可能剛好還沒被我們讀到，短暫輪詢；
        // 視窗外（Steam 純粹重讀舊值）就不碰暫存器，免得撿走別人的回應。
        pollMs = kFeatGetPollMs < pollBudgetMs ? kFeatGetPollMs : pollBudgetMs;
        n = pollFeatureResponse(cand, nc, reportId, expectType, pollMs,
                                resp, &respDev, &stolen);
    }
    m_featStolen += stolen;

    const Uint32 lat = SDL_GetTicks() - t0;
    int result;
    const char* src;
    if (n > 0) {
        result = 2;
        src = "live";
        m_featOk++;
        m_featLatSumMs += lat;
        if (lat > m_featLatMaxMs) m_featLatMaxMs = lat;

        // 快取：GET 退回時比 type；SET 退回時比整段 query（同 type 不夠——0xAE 這類
        // 帶參數的查詢，type 相同回應卻不同）。
        m_featCacheValid = true;
        m_featCacheTick = SDL_GetTicks();
        m_featCacheType = resp[1];
        memcpy(m_featCacheResp, resp, SC_HID_WIRE_BYTES);
        if (op == SC_OP_SET) {
            memcpy(m_featCacheQuery, qbuf, SC_HID_WIRE_BYTES);
        } else {
            // GET 撿到的即時回應不對應任何一筆我們送的 query：快取 query 清零，之後 SET
            // 逾時退回只會配到自己送過的 query（清零後 byte0=0 ≠ reportId，必不配對）。
            memset(m_featCacheQuery, 0, sizeof(m_featCacheQuery));
        }
        // 回應來自哪個 slot，控制器就在哪個 slot（readLoop 還沒看到 state 時很有用）
        if (m_activeDev.load() < 0) m_activeDev = respDev;
    } else {
        m_featEmpty++;
        bool useCache = false;
        // GET 且本場沒 SET 過：不接受任何回應（含快取），直接 empty。
        const bool cacheAllowed = !(op == SC_OP_GET && expectType == 0);
        if (cacheAllowed && m_featCacheValid &&
            (SDL_GetTicks() - m_featCacheTick) <= kFeatCacheTtlMs) {
            if (op == SC_OP_GET) {
                useCache = (m_featCacheType == expectType);
            } else {
                // 整段 64 bytes 比對（qbuf 與快取都零填充；不再另比長度）
                useCache = (memcmp(m_featCacheQuery, qbuf, SC_HID_WIRE_BYTES) == 0);
            }
        }
        if (useCache) {
            result = 1;
            src = "cache";
            m_featCache++;
            memcpy(resp, m_featCacheResp, SC_HID_WIRE_BYTES);
            resp[0] = reportId;
        } else {
            result = 0;
            src = "empty";
            memset(resp, 0, SC_HID_WIRE_BYTES);
            resp[0] = reportId;
        }
    }

    if (result > 0) {
        // stop() 進行中（LiStopConnection 可能已在跑）就不再往 control stream 送
        if (m_running) {
            LiSendScHidFeatureReport(seq, reportId, resp, SC_HID_WIRE_BYTES);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] Feature req%s id=0x%02x op=%s seq=%u type=0x%02x → resp type=0x%02x n=%d "
            "lat=%u ms dev=%d src=%s stolen=%u cand=%d sent=%d poll=%u ms",
            tag, reportId, opName(op), seq, expectType, resp[1], n, lat, respDev, src, stolen,
            nc, sentCount, pollMs);
    } else {
        // 一律不回零（SET / GET / 暖機皆同）：拿不到就不呼叫 LiSendScHidFeatureReport，
        // 讓 host driver 以 400 ms 閘控逾時退回 LastResponse。
        // 節流：控制器睡眠時 host Steam 每秒重試會洗版；總數在 rx stats 的 empty= 欄位。
        // 「GET 且本場沒 SET 過」是設計上的正常快速路徑（不輪詢），只在首次以 info 記一次。
        const bool quietPath = (op == SC_OP_GET && expectType == 0);
        if (quietPath) {
            if (m_featEmpty == 1) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SC-HID] Feature req%s id=0x%02x op=%s seq=%u: GET before any SET this session "
                    "— not polled, not replied (host driver keeps LastResponse)",
                    tag, reportId, opName(op), seq);
            }
        } else if (m_featEmpty == 1 || (m_featEmpty % 32) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] Feature req%s id=0x%02x op=%s seq=%u type=0x%02x → no response "
                "(lat=%u ms stolen=%u cand=%d sent=%d poll=%u ms empty#%u) — not replied; host driver "
                "falls back to LastResponse after its 400 ms gate",
                tag, reportId, opName(op), seq, expectType, lat, stolen, nc, sentCount, pollMs, m_featEmpty);
        }
    }
    return result;
}

bool ScHidPassthrough::prepare() {
    if (m_running || m_devCount > 0) return isPrepared();   // 已 prepare／已在跑

    // §HID-PROBE：探測若發現 Valve（0x28DE）介面對 control transfer 無回應，下面的
    // SDL_hid_enumerate / SDL_hid_open_path 以及之後讀取執行緒的 feature 查詢
    // （HidD_SetFeature／GetFeature = 同步 DeviceIoControl）全會在核心永久阻塞。
    // 本函式在主執行緒（Session::start()），卡住＝整個 UI 卡住，所以直接拒絕啟動。
    // 條件：VID 0x28DE（涵蓋 1302/1303/1304/1305 全家族），或 anyUnidentified()＝
    // 卡在 CreateFile／HidD_GetAttributes 階段或整個列舉逾時——已核對 SDL 3.4.2
    // src/hidapi/windows/hid.c hid_enumerate：對每個介面（不分 VID）先 open_device +
    // HidD_GetAttributes 再比 VID，只有相符者才查字串；所以其他廠商卡在字串查詢的裝置
    // 不會擋 SDL_hid_enumerate(0x28DE)，但卡在前兩步的會。升級 SDL 時重驗。
    if (HidProbe::isUnresponsiveVid(SC_VID) || HidProbe::anyUnidentified()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-HID] [SC-HID] passthrough refused: unresponsive Steam Controller / unidentified HID interface(s) %s - "
            "unplug and replug the Puck or controller, then start the stream again",
            HidProbe::describeUnresponsive().toUtf8().constData());
        return false;
    }
    if (HidProbe::anyUnresponsive()) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[VIPLE-HID] [SC-HID] non-Valve HID device(s) unresponsive at the string-query stage (%s); "
            "proceeding with 0x28DE-only enumeration",
            HidProbe::describeUnresponsive().toUtf8().constData());
    }

    if (SDL_hid_init() < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] SDL_hid_init failed: %s", SDL_GetError());
        return false;
    }

    // 新的一場：世代 +1，上一場若有被遺棄的執行緒，醒來看到世代不符就自行退出。
    m_gen++;
    resetStats();   // 每場歸零（含 seen mask 與 feature 佇列）
    if (m_abandoned) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] previous session abandoned a stuck read thread; starting a fresh generation (%u)",
            m_gen.load());
        m_abandoned = false;
    }

    // Find the Steam Controller's vendor gamepad interface across ANY transport
    // (USB-direct / Puck / Bluetooth) by enumerating VID 0x28DE and matching the
    // vendor usage, rather than a fixed product id.
    SDL_hid_device_info* devs = SDL_hid_enumerate(SC_VID, 0x0);

    int matched = 0;  // §SC-HID Linux 對齊：列舉到但開不了 = 權限問題可診斷
    const char* openedPaths[MAX_SC_DEVS] = {};   // Linux 後備條件下同一節點可能列舉多次，以 path 去重
    int opened = 0;
    for (SDL_hid_device_info* d = devs; d != nullptr && opened < MAX_SC_DEVS; d = d->next) {
        if (d->path == nullptr) continue;
        // 選擇條件（§SC-HID Linux 修正 2026-09-05）：
        //  (a) 標準：vendor collection UsagePage 0xFF00 / Usage 0x01——Windows/macOS 的 hidapi
        //      逐 top-level collection 列舉，Puck 的 4 個 slot 各是一筆。
        //  (b) Linux hidraw 後備：Ubuntu 26.04 的 SDL 2.32.10（真 SDL2，非 sdl2-compat）內建
        //      hidapi 在 Linux 不填 usage（回 0/0），且一個 hidraw 節點涵蓋該介面的全部
        //      collection（滑鼠 0x40 / 鍵盤 0x41 / vendor 共用同一 fd）。實測藍牙 SC 0x1303 只有
        //      一筆 `/dev/hidraw3 up=0 u=0`，舊條件永遠對不上 → 「No Steam Controller vendor
        //      interface found」。改以 VID 0x28DE + gen-2 PID 家族放行；滑鼠/鍵盤 report 由
        //      readLoop 的 id 過濾丟掉（計入 other）。
        const bool byUsage = (d->usage_page == SC_USAGE_PAGE && d->usage == SC_USAGE);
        const bool byPidUnknownUsage = (d->usage_page == 0 && d->usage == 0 && isGen2Pid(d->product_id));
        if (!byUsage && !byPidUnknownUsage) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] Skipping 28DE device pid=0x%04x if=%d usage=0x%04x/0x%04x path=%s (not the vendor collection)",
                d->product_id, d->interface_number, d->usage_page, d->usage, d->path);
            continue;
        }
        {
            bool dup = false;
            for (int k = 0; k < opened; k++) {
                if (openedPaths[k] && SDL_strcmp(openedPaths[k], d->path) == 0) { dup = true; break; }
            }
            if (dup) continue;
        }
        matched++;
        SDL_hid_device* h = SDL_hid_open_path(d->path, 0 /* shared */);
        if (h != nullptr) {
            SDL_hid_set_nonblocking(h, 1);  // non-blocking: we poll all handles
            m_devs[opened] = h;
            m_devPid[opened] = d->product_id;
            m_devIf[opened] = d->interface_number;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] Opened dev=%d pid=0x%04x if=%d usage=0x%04x/0x%04x%s path=%s",
                opened, d->product_id, d->interface_number, d->usage_page, d->usage,
                byUsage ? "" : " (linux hidraw fallback)", d->path);
            openedPaths[opened] = d->path;
            opened++;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] Open failed pid=0x%04x if=%d path=%s: %s",
                d->product_id, d->interface_number, d->path, SDL_GetError());
        }
    }
    // m_devCount 最後才發布：isPrepared() 在其他執行緒無鎖讀，發布時 m_devs[0..opened) 已全部就緒
    m_devCount = opened;
    if (devs != nullptr) {
        SDL_hid_free_enumeration(devs);
    }

    if (opened == 0) {
        if (matched > 0) {
            // §SC-HID Linux 對齊 2026-07-02：列舉得到（udev 列舉不需權限）
            // 但一個都開不起來 = 幾乎必是 /dev/hidraw* 權限（預設 0600
            // root-only）。給出可操作的提示，不要靜默降級。
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] Found %d Steam Controller vendor interface(s) but "
                "could not open any"
#ifdef __linux__
                " — likely missing hidraw permissions. Install the udev rule "
                "(see docs/setup_guide.md, scripts/linux/"
                "60-viplestream-steam-controller.rules) or the distro's "
                "steam-devices package, then replug the controller"
#endif
                " - Steam Controller stays a regular SDL gamepad this session", matched);
        }
        else {
            // No Steam Controller attached — that's fine
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] No Steam Controller vendor interface found (0x28DE FF00/01)");
        }
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SC-HID] Opened %d Steam Controller vendor interface(s) (prepared; SDL ignore list = %s)",
        opened, kSdlIgnoreDevicesSpec);
    return true;
}

void ScHidPassthrough::start() {
    if (m_running) return;
    if (m_devCount == 0) return;   // prepare() 沒開到裝置（已印過原因）

    // §SC-HID 暖機（Round 1 改為真查詢）：對實體 SC 送 SET 0x01 [0x83 GET_ATTRIBUTES_VALUES]
    // 再依 F13 時序輪詢取回應，成功就先塞給 server（seq=0），讓 host Steam 第一次查
    // GetControllerInfo 之前 driver 的 LastResponse 已是真資料。舊的 cold GET_FEATURE(0x01)
    // 在實體 SC 上必失（一次性暫存器沒有 SET 就是空的），所以從未印過 prime 訊息。
    // 總預算 kFeatWarmupBudgetMs（60 ms，含送 SET；硬上限）。§SC-THREAD-OWNER：排進佇列由
    // 讀取執行緒第一輪代跑——start() 在 session 建立路徑上，本身不再碰任何同步 IOCTL。
    // 失敗不回零給 host（避免把 driver 的 LastResponse 蓋成零）；正常握手仍由 host 請求即時代理。
    {
        FeatureReq warm = {};
        warm.reportId = SC_FEATURE_RID;
        warm.op = SC_OP_SET;
        warm.seq = 0;
        warm.query[0] = SC_FEATURE_RID;
        warm.query[1] = SC_MSG_GET_ATTRIBUTES_VALUES;  // FeatureReportHeader.type
        warm.query[2] = 0;                             // FeatureReportHeader.length
        warm.queryLen = 3;
        warm.budgetMs = kFeatWarmupBudgetMs;
        warm.tag = " (warm-up)";
        enqueueFeatureRequest(warm);
    }

    // The gen-2 Steam Controller needs NO enable command -- it streams its
    // reports by default. The earlier difficulty was reading the wrong (idle
    // Puck slot) interface; opening them all and polling each (above/below)
    // solves it. CAVEAT: if Steam Input is running locally it competes for the
    // same one-shot feature register (F14) — passthrough still works, but the
    // handshake response can be stolen; see `stolen` in the stats line.

    ScHidThreadCtx* ctx = new ScHidThreadCtx();
    ctx->self = this;
    ctx->gen = m_gen.load();
    ctx->simulateStall = (SDL_getenv("VIPLE_SC_HID_SIMULATE_STALL") != nullptr);
    if (ctx->simulateStall) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] VIPLE_SC_HID_SIMULATE_STALL set (dev-only): the read thread will pretend to be "
            "stuck in a HID call when the session stops, to exercise the abandon path");
    }
    m_threadCtx = ctx;
    m_running = true;
    m_thread = SDL_CreateThread(readThreadFunc, "SC-HID", ctx);
    if (!m_thread) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] Failed to create read thread: %s", SDL_GetError());
        m_running = false;
        m_threadCtx = nullptr;
        delete ctx;
        closeDevices();
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] Steam Controller passthrough started (normalize42=%d, gen=%u, "
            "all hidapi calls on the SC-HID thread)",
            kNormalize42 ? 1 : 0, ctx->gen);
    }
}

void ScHidPassthrough::stop() {
    if (m_running) {
        m_running = false;   // 讀取執行緒每個 hidapi 呼叫前都看它；正常情況 ≤ ~150 ms 退出
    }

    if (m_thread) {
        ScHidThreadCtx* ctx = m_threadCtx;
        const Uint32 t0 = SDL_GetTicks();
        while (!ctx->exited.load() && (SDL_GetTicks() - t0) < kStopJoinTimeoutMs) {
            SDL_Delay(kStopPollStepMs);
        }
        const Uint32 waited = SDL_GetTicks() - t0;
        if (ctx->exited.load()) {
            SDL_WaitThread(m_thread, nullptr);   // 已退出：立即返回
            m_thread = nullptr;
            m_threadCtx = nullptr;
            delete ctx;
            closeDevices();
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] Steam Controller passthrough stopped (read thread joined in %u ms)", waited);
        } else {
            // §SC-THREAD-OWNER：卡在核心的同步 IOCTL 從使用者態拉不回來（2026-09-19 Puck 事故；
            // 與 HidProbe 遺棄探測執行緒同一理由）。不 join、不 close handle（close 會讓本執行緒
            // 也跟著陪葬），detach 後連 ctx 一起漏掉；handle 指標清空、m_devCount 歸零，讓下一場
            // prepare() 從乾淨狀態開始——遺棄的執行緒醒來只會讀 m_running / m_gen 就退出。
            // 行程結束時 Windows 會等這條執行緒的 IRP 完成（可能留下殭屍到重插），
            // docs/troubleshooting.md 有寫。
            const int stuckOp = m_lastOp.load();
            const Uint32 stuckFor = SDL_GetTicks() - m_lastOpTick.load();
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-HID] [SC-HID] stop(): read thread did not exit within %u ms - stuck in %s for %u ms "
                "(the controller/Puck stopped answering); thread + %d HID handle(s) abandoned, not closed. "
                "Unplug and replug the device; the next stream start re-probes it",
                waited, lastOpName(stuckOp), stuckOp == OpIdle ? 0u : stuckFor, m_devCount.load());
            SDL_DetachThread(m_thread);
            m_thread = nullptr;
            m_threadCtx = nullptr;   // ctx 由遺棄的執行緒持有（漏掉）
            m_abandoned = true;
            const int count = m_devCount.load();
            m_devCount = 0;
            m_activeDev = -1;
            for (int i = 0; i < count; i++) {
                m_devs[i] = nullptr;   // 刻意不 SDL_hid_close
            }
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queueHead = 0;
            m_queueCount = 0;
        }
    } else if (m_devCount > 0) {
        // 只 prepare 沒 start（連線失敗 / 使用者取消）：把 handle 關掉
        closeDevices();
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] prepared Steam Controller interface(s) closed (session never started)");
    }
}

bool ScHidPassthrough::isActive() const {
    return m_running && m_devCount > 0;
}

int SDLCALL ScHidPassthrough::readThreadFunc(void* p) {
    ScHidThreadCtx* ctx = static_cast<ScHidThreadCtx*>(p);
    ctx->self->readLoop(ctx);
    ctx->exited = true;   // 之後不再碰 self（正常路徑 stop() 會 delete ctx）
    return 0;
}

void ScHidPassthrough::forwardFeatureRequest(uint8_t reportId, uint8_t op, uint8_t seq,
                                             const uint8_t* query, uint8_t queryLen) {
    // §SC-THREAD-OWNER：本函式在 common-c control stream 的 async callback 執行緒上被呼叫，
    // 只把請求排進佇列（≤ 1 ms 後由讀取執行緒代跑），絕不碰 hidapi——卡死的裝置永遠
    // 只會困住我們自己的執行緒，不會困住 common-c 的（LiStopConnection 要 join 它）。
    if (!m_running || m_devCount == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] forwardFeatureRequest: no device open (id=0x%02x op=%s seq=%u)",
            reportId, opName(op), seq);
        return;
    }

    // §SC-HID Phase 2C — transparent proxy：把 host Steam 對虛擬裝置做的 SET/GET
    // 原樣重放到實體 SC，回應帶同一個 seq 走 0x55000008 回 host；時序依 F13
    // （SET → 每 1 ms 輪詢 GET；type 比對；GET op 只在 SET 後 400 ms 視窗內短暫
    // 輪詢，否則退回 5 s 內快取）。拿不到一律不回零——host driver 400 ms 閘控逾時
    // 會自己退回 LastResponse。
    FeatureReq req = {};
    req.reportId = reportId;
    req.op = op;
    req.seq = seq;
    int qn = queryLen;
    if (qn > SC_HID_WIRE_BYTES) qn = SC_HID_WIRE_BYTES;
    if (query && qn > 0) memcpy(req.query, query, (size_t)qn);
    req.queryLen = (uint8_t)qn;
    req.budgetMs = kFeatSetPollMs;
    req.tag = "";
    enqueueFeatureRequest(req);
}

// 處理一筆讀到的 input report（讀取執行緒）。回傳 true = 已轉發給 host。
bool ScHidPassthrough::handleInputReport(int dev, uint8_t* buf, int n) {
    const uint8_t id = buf[0];
    m_rxTotal++;
    m_devRx[dev]++;

    // 首見 id：印全部 bytes（hex）。用 APPLICATION 類別——app 從未設 SDL log
    // 等級，INPUT 類別預設不印（舊碼的丟棄 log 就是這樣消失的）。
    const bool firstSeen = !(m_seenMask[id >> 5] & (1u << (id & 31)));
    if (firstSeen) {
        m_seenMask[id >> 5] |= (1u << (id & 31));
        char hex[SC_HID_WIRE_BYTES * 3 + 1];
        hexDump(hex, sizeof(hex), buf, n);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] First report id=0x%02x on dev=%d (%d bytes): %s", id, dev, n, hex);
    }

    bool forward = false;
    switch (id) {
    case SC_RID_STATE:
        // Puck / USB 直連的 gamepad state。與 0x45 同 TritonMTUNoQuat_t 佈局，
        // 改標 0x45 讓 host 虛擬裝置照現有 descriptor 交給 Steam 解析。
        m_rxId42++;
        m_activeDev = dev;
        if (kNormalize42) {
            buf[0] = SC_RID_STATE_BLE;
            m_rxNorm42++;
        }
        forward = true;
        break;
    case SC_RID_STATE_BLE:
        m_rxId45++;
        m_activeDev = dev;
        forward = true;
        break;
    case SC_RID_STATE_TS:
        // 佈局是 TritonMTUNoQuat32TS_t（16-bit timestamp），改 id 直送會讓 Steam
        // 解錯欄位；硬體實測也沒看到，本輪不轉發。
        m_rxId47++;
        if (firstSeen) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] id=0x47 STATE_TIMESTAMP seen on dev=%d — different layout "
                "(TritonMTUNoQuat32TS_t), not forwarded in Round 1", dev);
        }
        break;
    case SC_RID_BATTERY:
        m_rxId43++;
        break;
    case SC_RID_TELEMETRY:
        m_rxId7B++;
        break;
    case SC_RID_WIRELESS_X:
    case SC_RID_WIRELESS: {
        // byte1 = ETritonWirelessState（1 disconnect / 2 connect）：記 slot 連線狀態
        m_rxIdOther++;
        if (n >= 2) {
            const uint8_t state = buf[1];
            if (state != m_devLink[dev]) {
                m_devLink[dev] = state;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[SC-HID] Wireless status id=0x%02x dev=%d state=%u (%s)",
                    id, dev, state, wirelessName(state));
                if (state == SC_WIRELESS_DISCONNECT && m_activeDev.load() == dev) {
                    m_activeDev = -1;  // 這個 slot 的控制器走了，feature 請求改回逐一嘗試
                }
            }
        }
        break;
    }
    default:
        m_rxIdOther++;
        break;
    }

    if (!forward) {
        m_rxDrop++;
        return false;
    }

    // Pad short reads to exactly the fixed wire report size.
    if (n < SC_HID_WIRE_BYTES) {
        memset(buf + n, 0, SC_HID_WIRE_BYTES - n);
    }
    const int rc = LiSendScHidInputReport(buf, SC_HID_WIRE_BYTES);
    if (rc != 0) {
        m_rxSendErr++;
        if (m_rxSendErr == 1 || (m_rxSendErr % 1000) == 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] LiSendScHidInputReport failed rc=%d (count=%u, dev=%d)",
                rc, m_rxSendErr, dev);
        }
        return false;  // 沒真的送出去，這輪不算轉發
    }
    m_rxFwd++;
    m_devFwd[dev]++;
    return true;
}

void ScHidPassthrough::readLoop(ScHidThreadCtx* ctx) {
    uint8_t buf[SC_HID_WIRE_BYTES];
    const uint32_t myGen = ctx->gen;
    // alive()：本場仍在跑且世代未變。stop() 遺棄本執行緒後，任何醒來的路徑都以此為準——
    // 世代不符就一路 return，不再讀寫任何成員（新一場可能已在用同一塊記憶體）。
    auto alive = [this, myGen]() { return m_running.load() && m_gen.load() == myGen; };

    const Uint32 startTick = SDL_GetTicks();
    Uint32 lastStatsTick = startTick;   // 上次評估要不要印統計
    Uint32 lastPrintTick = startTick;   // 上次真的印出（心跳用）
    uint32_t printedTotal = 0;          // 上次印出時的 m_rxTotal / m_featReq（有變才印）
    uint32_t printedFeat = 0;
    bool warnedNoPackets = false;

    while (alive()) {
        // gotAny 語義（Round 1）：只有「本輪真的有轉發」才不讓步；只讀到雜訊
        // （電量 / 遙測 / 無線狀態）的輪次仍 SDL_Delay(1)，避免空轉吃 CPU。
        bool forwardedAny = false;

        // Poll every opened vendor interface; only the active controller's
        // interface actually delivers reports (the others stay silent).
        // read 是 non-blocking（prepare() 已 SDL_hid_set_nonblocking），健康裝置不會久待。
        const int count = m_devCount.load();
        for (int i = 0; i < count; i++) {
            if (!alive()) return;
            m_lastOp = OpRead;
            m_lastOpTick = SDL_GetTicks();
            const int n = SDL_hid_read(m_devs[i], buf, sizeof(buf));
            m_lastOp = OpIdle;
            if (n <= 0) {
                continue;  // 0 = no data (non-blocking); <0 = error on this handle
            }
            if (handleInputReport(i, buf, n)) {
                forwardedAny = true;
            }
        }

        // §SC-THREAD-OWNER：每輪 sweep 代跑一筆佇列裡的 feature 請求（≤ kFeatSetPollMs）。
        // 期間實體 SC 的 input report 由 OS HIDClass ring buffer 暫存（SDL hidapi 開 handle
        // 時已 HidD_SetNumInputBuffers(64)，266 Hz × 100 ms ≈ 27 筆），不會掉資料。
        // 一輪只跑一筆，讓讀取與 feature 交錯；host driver 的 400 ms 閘控本來就把握手序列化。
        {
            FeatureReq req;
            if (alive() && dequeueFeatureRequest(&req)) {
                runFeatureRequest(req.reportId, req.op, req.seq, req.query, req.queryLen,
                                  req.budgetMs, req.tag);
                if (!alive()) return;
            }
        }

        const Uint32 now = SDL_GetTicks();

        // 啟動後 kNoPacketWarnMs 仍一筆都沒有 → 一次 warning（控制器睡眠 / 沒對上 slot）
        if (!warnedNoPackets && m_rxTotal == 0 && (now - startTick) >= kNoPacketWarnMs) {
            warnedNoPackets = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] No input report from any of %d dev(s) in %u ms — controller "
                "asleep (press the Steam button) or not paired to an opened slot",
                m_devCount.load(), (unsigned)(now - startTick));
        }

        // 每 kStatsPeriodMs 評估：有變才印；沒變也每 kHeartbeatMs 印一次心跳
        if ((now - lastStatsTick) >= kStatsPeriodMs) {
            lastStatsTick = now;
            const bool changed = (m_rxTotal != printedTotal) || (m_featReq != printedFeat);
            if (changed || (now - lastPrintTick) >= kHeartbeatMs) {
                logRxStats(changed ? "5s" : "heartbeat");
                lastPrintTick = now;
                printedTotal = m_rxTotal;
                printedFeat = m_featReq;
            }
        }

        if (!forwardedAny) {
            SDL_Delay(1);  // nothing forwarded this sweep -- yield briefly
        }
    }

    if (m_gen.load() != myGen) return;   // 被遺棄後才醒來：什麼都別碰

    if (ctx->simulateStall) {
        // dev-only：假裝卡在核心——使用者態 Sleep 是可終止的（行程結束不會留殭屍），
        // 只用來驗證 stop() 的逾時／遺棄／下一場重新 prepare 路徑。
        m_lastOp = OpGetFeature;
        m_lastOpTick = SDL_GetTicks();
        for (;;) SDL_Delay(1000);
    }

    // 最終統計（與 host 的 `Session ended, final write stats` 對帳）——stop() 還在等我們，
    // m_devs/m_devPid 仍有效。
    logRxStats("final");
}
