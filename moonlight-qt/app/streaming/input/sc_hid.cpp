// §SC-HID: Steam Controller raw-HID passthrough (client side)

#include "sc_hid.h"
#include "SDL_compat.h"
#include <SDL_hidapi.h>
#include <cstring>   // memset / memcpy

extern "C" {
#include <Limelight.h>
}

// Fixed wire report size (SS_SC_HID_REPORT_MAX in moonlight-common-c's internal
// Input.h, which is not on the app include path). LiSendScHidInputReport always
// sends exactly this many bytes.
static constexpr int SC_HID_WIRE_BYTES = 64;

// gen-2 SC 的 vendor gamepad 串流 report id（server 端虛擬裝置的 report
// descriptor 只宣告這個 input id；其他 id 是 puck 無線狀態等雜訊，不轉發）
static constexpr uint8_t SC_REPORT_ID_GAMEPAD = 0x45;

// Valve vendor id. The gen-2 (2025) Steam Controller exposes its gamepad data
// on a vendor-defined HID interface (UsagePage 0xFF00 / Usage 0x01) carrying
// HID report id 66 (0x42). We select the device by that usage rather than by a
// hard-coded product id, so all transports are handled by one code path:
//   0x1302 = USB-direct, 0x1303 = Bluetooth LE, 0x1304 = Puck (wireless).
static constexpr unsigned short SC_VID        = 0x28DE;
static constexpr unsigned short SC_USAGE_PAGE = 0xFF00;
static constexpr unsigned short SC_USAGE      = 0x0001;

ScHidPassthrough::ScHidPassthrough() = default;

ScHidPassthrough::~ScHidPassthrough() {
    stop();
}

void ScHidPassthrough::start() {
    if (m_running) return;

    if (SDL_hid_init() < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] SDL_hid_init failed: %s", SDL_GetError());
        return;
    }

    // Find the Steam Controller's vendor gamepad interface across ANY transport
    // (USB-direct / Puck / Bluetooth) by enumerating VID 0x28DE and matching the
    // vendor usage, rather than a fixed product id.
    SDL_hid_device_info* devs = SDL_hid_enumerate(SC_VID, 0x0);

    // §SC-DEV-LOCK-FIX (review batch 2)：LiStartConnection 成功後 control
    // stream 已在跑，clScHidFeatureRequest 可能在 start() 進行中抵達——
    // m_devs/m_devCount 的填充、暖機 get_feature 與失敗清理都要在鎖內。
    // SDL_CreateThread 在鎖內呼叫無妨——readLoop 第一輪會等鎖釋放。
    std::lock_guard<std::mutex> lock(m_devMutex);
    m_devCount = 0;
    int matched = 0;  // §SC-HID Linux 對齊：列舉到但開不了 = 權限問題可診斷
    for (SDL_hid_device_info* d = devs; d != nullptr && m_devCount < MAX_SC_DEVS; d = d->next) {
        if (d->usage_page == SC_USAGE_PAGE && d->usage == SC_USAGE && d->path != nullptr) {
            matched++;
            SDL_hid_device* h = SDL_hid_open_path(d->path, 0 /* shared */);
            if (h != nullptr) {
                SDL_hid_set_nonblocking(h, 1);  // non-blocking: we poll all handles
                m_devs[m_devCount++] = h;
            }
        }
    }
    if (devs != nullptr) {
        SDL_hid_free_enumeration(devs);
    }

    if (m_devCount == 0) {
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
                , matched);
        }
        else {
            // No Steam Controller attached — that's fine
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] No Steam Controller vendor interface found (0x28DE FF00/01)");
        }
        return;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SC-HID] Opened %d Steam Controller vendor interface(s)", m_devCount.load());

    // §SC-HID Phase 2C 暖機：向伺服器端主動送一次 feature 0x01 快取，讓 server
    // 在 Steam 第一次查 GetControllerInfo 之前就有真實 SC 的 firmware 資料。
    // 即使實體 SC 不支援 cold GET_FEATURE(0x01) 這也是 best-effort — worst case
    // 是送一包 zeros，proxy 的正常 SET+GET 輪回仍能在數百 ms 內完成真正填充。
    for (int i = 0; i < m_devCount; i++) {
        uint8_t buf[SC_HID_WIRE_BYTES] = {};
        buf[0] = 0x01;  // feature report id
        int n = SDL_hid_get_feature_report(m_devs[i], buf, sizeof(buf));
        if (n > 0) {
            if (n < SC_HID_WIRE_BYTES) memset(buf + n, 0, SC_HID_WIRE_BYTES - n);
            LiSendScHidFeatureReport(0 /*seq*/, 0x01 /*reportId*/, buf, SC_HID_WIRE_BYTES);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] Proactive cache prime sent (dev=%d, %d bytes)", i, n);
            break;
        }
    }

    // The gen-2 Steam Controller needs NO enable command -- it streams its
    // reports by default. The earlier difficulty was reading the wrong (idle
    // Puck slot) interface; opening them all and polling each (above/below)
    // solves it. CAVEAT: if Steam Input is running locally it can claim or
    // reconfigure the controller (changing which report it streams), which
    // interferes with passthrough -- the local machine should not be running
    // Steam on this controller. See project memory project-sc-hid-passthrough.

    m_running = true;
    m_thread = SDL_CreateThread(readThreadFunc, "SC-HID", this);
    if (!m_thread) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] Failed to create read thread: %s", SDL_GetError());
        m_running = false;
        for (int i = 0; i < m_devCount; i++) {
            SDL_hid_close(m_devs[i]);
            m_devs[i] = nullptr;
        }
        m_devCount = 0;
    } else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] Steam Controller passthrough started");
    }
}

void ScHidPassthrough::stop() {
    if (!m_running) return;
    m_running = false;

    // Reads are non-blocking, so the thread exits on its own once m_running is
    // cleared. Join first, then close the handles it was polling.
    // 順序鐵律：先 SDL_WaitThread 再拿鎖——若先拿鎖再 join，readLoop
    // 卡在搶鎖會 deadlock。
    if (m_thread) {
        SDL_WaitThread(m_thread, nullptr);
        m_thread = nullptr;
    }

    // §SC-DEV-LOCK-FIX (review batch 2)：close 必須在 m_devMutex 內做。
    // stop() 在 LiStopConnection 之前被呼叫（session.cpp cleanup task），
    // common-c 的 async callback 執行緒此時還活著，forwardFeatureRequest
    // 仍可能抵達；持鎖 + 它在鎖內檢查 m_running/m_devCount，保證不會
    // use-after-close。
    {
        std::lock_guard<std::mutex> lock(m_devMutex);
        for (int i = 0; i < m_devCount; i++) {
            if (m_devs[i]) {
                SDL_hid_close(m_devs[i]);
                m_devs[i] = nullptr;
            }
        }
        m_devCount = 0;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
        "[SC-HID] Steam Controller passthrough stopped");
}

bool ScHidPassthrough::isActive() const {
    return m_running && m_devCount > 0;
}

int SDLCALL ScHidPassthrough::readThreadFunc(void* ctx) {
    static_cast<ScHidPassthrough*>(ctx)->readLoop();
    return 0;
}

void ScHidPassthrough::forwardFeatureRequest(uint8_t reportId, uint8_t op, uint8_t seq,
                                             const uint8_t* query, uint8_t queryLen) {
    // §SC-DEV-LOCK-FIX (review batch 2)：整段持鎖——與 readLoop / stop()
    // 互斥。SET 路徑的 SDL_Delay(20) 也在鎖內：feature op 只在 Steam 握手
    // 時出現（低頻），期間實體 SC 的 input report 由 OS HIDClass ring
    // buffer 暫存，不會掉資料。鎖內檢查 m_running 防 stop() 後的
    // use-after-close（stop 先 join read thread 再持鎖 close handle）。
    std::lock_guard<std::mutex> lock(m_devMutex);
    if (!m_running || m_devCount == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
            "[SC-HID] forwardFeatureRequest: no device open");
        return;
    }

    // §SC-HID Phase 2C — transparent proxy. We replay EXACTLY what Steam did on
    // the host against the real SC, so any GetControllerInfo query sequence works
    // without hard-coding command bytes:
    //   op==2 (SET): write Steam's query to the SC, then read its feature report.
    //   op==1 (GET): just read the SC's current feature report.
    // The first interface that yields a response wins; its bytes go back tagged
    // with the same seq via the dedicated feature channel (0x55000008).
    uint8_t buf[SC_HID_WIRE_BYTES];

    for (int i = 0; i < m_devCount; i++) {
        if (op == 2 /* SET */) {
            uint8_t qbuf[SC_HID_WIRE_BYTES] = {};
            qbuf[0] = reportId;
            int qn = queryLen;
            if (qn > SC_HID_WIRE_BYTES) qn = SC_HID_WIRE_BYTES;
            if (query && qn > 0) memcpy(qbuf, query, qn);
            qbuf[0] = reportId;   // ensure report id is correct in byte 0
            SDL_hid_send_feature_report(m_devs[i], qbuf, sizeof(qbuf));
            SDL_Delay(20);        // let the SC process the query before reading back
        }

        memset(buf, 0, sizeof(buf));
        buf[0] = reportId;
        int n = SDL_hid_get_feature_report(m_devs[i], buf, sizeof(buf));

        if (n > 0) {
            if (n < SC_HID_WIRE_BYTES) {
                memset(buf + n, 0, SC_HID_WIRE_BYTES - n);
            }
            LiSendScHidFeatureReport(seq, reportId, buf, SC_HID_WIRE_BYTES);
            SDL_LogDebug(SDL_LOG_CATEGORY_APPLICATION,
                "[SC-HID] Feature response forwarded: id=0x%02x op=%u seq=%u (%d bytes)",
                reportId, op, seq, n);
            return;  // first successful response is enough
        }
    }

    // No interface produced a response. Still answer the host (zeros) so its
    // seq round-trip completes and Steam's GET_FEATURE doesn't hang on a retry.
    memset(buf, 0, sizeof(buf));
    buf[0] = reportId;
    LiSendScHidFeatureReport(seq, reportId, buf, SC_HID_WIRE_BYTES);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
        "[SC-HID] forwardFeatureRequest: GET 0x%02x empty on all %d device(s) (op=%u seq=%u)",
        reportId, m_devCount.load(), op, seq);
}

void ScHidPassthrough::readLoop() {
    uint8_t buf[64];

    while (m_running) {
        bool gotAny = false;

        {
            // §SC-DEV-LOCK-FIX (review batch 2)：每輪 sweep 持鎖，與
            // forwardFeatureRequest / stop() 互斥。read 是 non-blocking
            // （start() 已 SDL_hid_set_nonblocking），不會在鎖內久待；
            // SDL_Delay 留在鎖外，feature 請求不會被餓死。
            std::lock_guard<std::mutex> lock(m_devMutex);

            // Poll every opened vendor interface; only the active controller's
            // interface actually delivers reports (the others stay silent).
            for (int i = 0; i < m_devCount; i++) {
                int n = SDL_hid_read(m_devs[i], buf, sizeof(buf));
                if (n <= 0) {
                    continue;  // 0 = no data (non-blocking); <0 = error on this handle
                }
                gotAny = true;

                // §SC-HID 0x45 filter 2026-07-02：只轉發 gamepad 流（report id
                // 0x45）。同機的 puck（PID 0x1304）閒置 slot 介面會吐無線狀態
                // 封包（實測 server 端曾注入到 id=0x42）——虛擬裝置的 report
                // descriptor 只宣告 input 0x45，把雜訊灌進去只會干擾 Steam
                // 對虛擬裝置的解讀。非 0x45 的 report 丟棄（每個 id 首見時
                // log 一次供診斷）。
                if (buf[0] != SC_REPORT_ID_GAMEPAD) {
                    static uint32_t seenMask[8];  // id 0..255 bitmap
                    uint8_t id = buf[0];
                    if (!(seenMask[id >> 5] & (1u << (id & 31)))) {
                        seenMask[id >> 5] |= (1u << (id & 31));
                        SDL_LogInfo(SDL_LOG_CATEGORY_INPUT,
                                    "[SC-HID] Skipping non-gamepad report id=0x%02x "
                                    "(dev=%d, %d bytes) — not forwarding",
                                    id, i, n);
                    }
                    continue;
                }

                // Pad short reads to exactly the fixed wire report size.
                if (n < SC_HID_WIRE_BYTES) {
                    memset(buf + n, 0, SC_HID_WIRE_BYTES - n);
                }
                LiSendScHidInputReport(buf, SC_HID_WIRE_BYTES);
            }
        }

        if (!gotAny) {
            SDL_Delay(1);  // nothing pending on any interface -- yield briefly
        }
    }
}
